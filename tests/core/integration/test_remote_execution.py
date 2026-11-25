"""Opt-in execution against a disposable Linux tsugumi-workerd.

Set TSUGUMI_REMOTE_TEST_DAEMON to start packaged tsugumi-workerd on loopback.
For a daemon already running, set TSUGUMI_REMOTE_TEST_ENDPOINT and
TSUGUMI_REMOTE_TEST_PUBLIC_KEY_FILE, plus TSUGUMI_REMOTE_TEST_AUTHORIZED_KEYS
pointing to that worker's dedicated writable authorized-keys file.
Use tsugumi-test-env and pytest -m environment.
"""

import errno
import json
import os
import secrets
import shutil
import socket
import socketserver
import struct
import tempfile
import subprocess
import threading
import time
from pathlib import Path

import pytest
from harness import remote_worker as R
from Tsugumi.RemoteWorker.WorkerReady import WorkerReady
from harness.file_events import process_file
from harness.host import Host
from harness.real_cards import PYTHON_BINDINGS, log, python_run
from harness.values import collected_items
from harness.views import open_view, return_credit
from projects import (
    card,
    conn,
    run_and_wait,
    start_run,
    wait_for_display,
    wait_invocation,
    wait_run,
    write_nix_project,
)
from Tsugumi.Data.CardState import CardState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RemoteExecutionState import RemoteExecutionState
from Tsugumi.Data.RunState import RunState
from Tsugumi.Trace.Client import Client
from tsugumi_client import path, text

pytestmark = pytest.mark.environment

RUN_LIST_OUTPUT = 'table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;'
# Each remote job realizes its dependencies in a private store, even during a batch.
RUN_LIST_TIMEOUT_SECONDS = 1200
RUN_LIST_REMOTE = {
    "type": "code", "recordIO": "on", "workerCapabilities": [63],
    "timeoutSeconds": RUN_LIST_TIMEOUT_SECONDS, "_outputs": RUN_LIST_OUTPUT,
}


def write(project, definitions, cards, connections, *, files=None):
    remote_system = os.environ["TSUGUMI_TEST_REMOTE_SYSTEM"]
    definitions = {
        key: {"systems": [remote_system], **body} if body.get("workerCapabilities") else body
        for key, body in definitions.items()
    }
    write_nix_project(project, definitions, cards, connections, files=files)


@pytest.fixture(scope="session")
def remote_store_seed(tmp_path_factory):
    manifest = os.environ.get("TSUGUMI_REMOTE_TEST_SEED_MANIFEST")
    if not manifest or not os.environ.get("TSUGUMI_REMOTE_TEST_DAEMON"):
        return None
    seed = tmp_path_factory.mktemp("remote-store-seed")
    subprocess.run(
        ["nix", "copy", "--no-check-sigs", "--option", "build-users-group", "",
         "--to", f"local?root={seed}", str(Path(manifest).resolve())],
        check=True,
    )
    return seed


@pytest.fixture
def remote_daemon(tmp_path, request, remote_store_seed):
    binary = os.environ.get("TSUGUMI_REMOTE_TEST_DAEMON")
    if not binary:
        if getattr(request, "param", None):
            pytest.skip("worker settings tests require a disposable packaged tsugumi-workerd")
        endpoint = os.environ.get("TSUGUMI_REMOTE_TEST_ENDPOINT")
        key_file = os.environ.get("TSUGUMI_REMOTE_TEST_PUBLIC_KEY_FILE")
        if not endpoint or not key_file:
            pytest.skip("set TSUGUMI_REMOTE_TEST_DAEMON or a dedicated endpoint and public key file")
        yield endpoint, key_file, None
        return

    port = 0
    state = tmp_path / "daemon-state"
    state.mkdir(mode=0o700)
    if remote_store_seed is not None:
        store = state / "store"
        store.mkdir(mode=0o700)
        (store / "nix").mkdir()
        # Reuse immutable payloads. Each daemon gets independent Nix metadata,
        # job state and authorization, and builds its own Card-specific paths.
        subprocess.run(["cp", "-al", remote_store_seed / "nix/store", store / "nix/store"], check=True)
        shutil.copytree(remote_store_seed / "nix/var", store / "nix/var", symlinks=True)
    env = {**os.environ, "TSUGUMI_REMOTE_STATE_DIR": str(state)}
    # The listener's private socket defaults to a path inside its state folder,
    # which under pytest's temporary folder can exceed the socket path limit.
    sockets = tempfile.TemporaryDirectory(prefix="w-", dir=os.environ.get("TSUGUMI_SOCKET_DIR", "/tmp"))
    request.addfinalizer(sockets.cleanup)
    command = [binary, "--host", "127.0.0.1", "--port", str(port),
               "--socket", str(Path(sockets.name) / "w.sock"),
               *getattr(request, "param", ())]
    key_file = tmp_path / "worker-public-key"
    key_file.write_bytes(subprocess.check_output([binary, "--public-key"], env=env))
    daemon_log = tmp_path / "execution-daemon.log"
    process = None

    def start():
        nonlocal process, port
        ready = state / "worker.ready"
        ready.unlink(missing_ok=True)
        with daemon_log.open("ab") as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        published = process_file(process, ready)
        if published is None:
            raise RuntimeError(daemon_log.read_text())
        port = WorkerReady.GetRootAs(published.read_bytes()).Port()
        command[command.index("--port") + 1] = str(port)

    def restart():
        process.kill()
        process.wait()
        start()

    try:
        start()
        yield f"tls://127.0.0.1:{port}", str(key_file), restart
    finally:
        if process and process.poll() is None:
            process.terminate()
            # Escalate only if the disposable listener ignores shutdown.
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


@pytest.fixture
def remote_broker(project, tmp_path, monkeypatch, remote_daemon):
    endpoint, key_file, _ = remote_daemon
    copy = tmp_path / "copied-project"
    shutil.copytree(project, copy)
    host = Host(tmp_path / "agent")
    external_authorized = None
    added_key = None
    try:
        public = R.string(host.call("ExecutionServicesRequest").hostPublicKey)
        if os.environ.get("TSUGUMI_REMOTE_TEST_DAEMON"):
            (tmp_path / "daemon-state/authorized_keys").write_text(public + "\n")
        else:
            authorized = os.environ.get("TSUGUMI_REMOTE_TEST_AUTHORIZED_KEYS")
            assert authorized, "external test worker needs a dedicated writable authorized_keys file"
            external_authorized = Path(authorized)
            previous = external_authorized.read_text() if external_authorized.exists() else ""
            added_key = public
            external_authorized.write_text(previous.rstrip("\n") + "\n" + public + "\n")
        host.call("ExecutionServicePutRequest", id="remote-test", name="Remote test",
                  endpoint=endpoint, workerPublicKey=Path(key_file).read_text().strip(), verify=True,
                  workerCapabilities=[63])
        service = next(value for value in host.call("ExecutionServicesRequest").services
                       if text(value.id) == "remote-test")
        monkeypatch.setenv("TSUGUMI_TEST_REMOTE_SYSTEM", text(service.capabilities.nativeSystem))

        def authorize(folder, name):
            control = tmp_path / name
            control.mkdir(mode=0o700)
            result = host.call("ExecutionProjectAuthorizeRequest", projectId=name,
                               path=str(folder), controlDir=str(control))
            assert result.available, "test filesystem must provide directory birth time"
            return R.string(result.capabilityFile)

        ready = {"endpoint": host.ready["execution_broker"],
                 "capability": authorize(project, "control"),
                 "copyCapability": authorize(copy, "copy-control"),
                 "host": host}
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER", ready["endpoint"])
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE", ready["capability"])
        yield ready
    finally:
        host.close()
        if external_authorized is not None and added_key is not None:
            # Remove only this fixture's key, preserving other hosts and edits.
            lines = external_authorized.read_text().splitlines(keepends=True)
            external_authorized.write_text("".join(line for line in lines if line.strip() != added_key))


def broker_request(endpoint, capability_file, body_type, body, **routing):
    envelope = R.broker_request(Path(capability_file).read_text().strip(), body_type, body,
                                service="remote-test", **routing)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(30)
        address = endpoint.removeprefix("unix:")
        connection.connect(address + ".c" if body_type == R.RequestBody.CancelRequest else address)
        connection.sendall(R.frame(envelope))
        with connection.makefile("rb") as response:
            return R.read(response).payload


@pytest.fixture
def remote_results(remote_broker, monkeypatch, tmp_path):
    """Observe the disposable worker's result set before the core releases it."""
    upstream = remote_broker["endpoint"].removeprefix("unix:")
    results = {}

    class Forward(socketserver.BaseRequestHandler):
        def handle(self):
            with self.request.makefile("rb") as downstream, socket.socket(socket.AF_UNIX) as connection:
                header = R.exact(downstream, 4)
                size, = struct.unpack("<I", header)
                payload = R.exact(downstream, size)
                request = R.MessageT.InitFromPackedBuf(payload).payload.request
                if request.bodyType == R.RequestBody.GetBlobRequest:
                    job = text(request.jobId)
                    folders = list((tmp_path / "daemon-state/projects").glob(f"*/jobs/{job}/work/result"))
                    if folders and job not in results:
                        directory, = folders
                        results[job] = {file.relative_to(directory).as_posix(): file.read_bytes()
                                        for file in directory.rglob("*") if file.is_file()}
                control = request.bodyType == R.RequestBody.CancelRequest
                connection.connect(upstream + ".c" if control else upstream)
                connection.sendall(header + payload)
                with connection.makefile("rb") as response:
                    header = R.exact(response, 4)
                    size, = struct.unpack("<I", header)
                    self.request.sendall(header + R.exact(response, size))

    with tempfile.TemporaryDirectory(prefix="rr-", dir=os.environ.get("TSUGUMI_SOCKET_DIR")) as sockets:
        address = str(Path(sockets) / "broker")
        with socketserver.ThreadingUnixStreamServer(address, Forward) as server:
            os.symlink(address, address + ".c")
            monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER", "unix:" + address)
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                yield results
            finally:
                server.shutdown()
                thread.join()


GATE_HELPER = """
def wait_for_release():
    import os
    os.mkfifo("test-release")
    os.chmod("test-release", 0o666)
    with open("test-release") as gate:
        gate.readline()
"""


def entry(run, extra=""):
    return "{ params, inputs }: let " + PYTHON_BINDINGS + " in { run = " + run + "; " + extra + " }"


@pytest.fixture
def remote_gates(tmp_path, remote_daemon):
    if remote_daemon[2] is None:
        # The externally managed worker exposes status but has no fixture
        # channel for releasing a staged Card inside its sandbox.
        return None

    def release(invocation, *, before_release=None):
        job = text(invocation.executionJobId)
        directory, = (tmp_path / "daemon-state/projects").glob(f"*/jobs/{job}")
        fifo = directory / "work/scratch/work/test-release"
        # The remote job reaches Running before its private store finishes
        # building the Card. Wait for the Card to actually open the gate.
        deadline = time.monotonic() + 180
        while True:
            try:
                fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                break
            except OSError as error:
                if error.errno not in (errno.ENOENT, errno.ENXIO):
                    raise
                if time.monotonic() >= deadline:
                    pytest.fail(f"remote job {job} did not open its release gate: {fifo}")
                time.sleep(0.05)
        with os.fdopen(fd, "w") as gate:
            if before_release is not None:
                before_release()
            gate.write("release\n")

    return release


def fixture(project):
    schema = "table Text { value:string; } table Outputs { doc:Text (required); } root_type Outputs;"
    body = GATE_HELPER + """
import os
import platform
from pathlib import Path
from card import Output
from Text import TextT

def run(input):
    wait_for_release()
    assert platform.system() == "Linux"
    assert not Path("/etc/hostname").exists()
    assert "TSUGUMI_REMOTE_PRIVATE_KEY" not in os.environ
    print("remote-card-stdout")
    return Output(doc=TextT(value=input.require("in").value))
"""
    source = {"type": "code", "_params": '{ value = "remote-input-roundtrip"; }',
              "_outputs": schema, "_nix": entry(python_run("""
from card import Output, params
from Text import TextT


def run(input):
    return Output(doc=TextT(value=params.value))
"""))}
    settings = {
        "type": "code", "inputs": [{"name": "in"}], "_outputs": schema,
        "workerCapabilities": [63], "recordIO": "on", "timeoutSeconds": 600,
    }
    definitions = {
        "input": source,
        "slow": {**settings, "_nix": entry(python_run(body))},
    }
    write(project, definitions, [card("source", "input"), card("slow", "slow")],
          [conn("source", "doc", "slow", "in")])


def check_success(client, run_id, name, system):
    record = client.call("RunRecordRequest", runId=run_id, card=path(name)).record
    summary = record.summary
    assert summary.state == CardState.succeeded, summary.failure and text(summary.failure.diagnostic)
    assert summary.executionState == RemoteExecutionState.succeeded
    assert text(summary.executionService) == "remote-test"
    assert text(summary.executionSystem) == system
    received, = record.inputs
    sent, = record.outputs
    assert bytes(sent.inlineBytes) == bytes(received.inlineBytes)
    logs = client.call("LogRangeRequest", runId=run_id, card=path(name), invocation=1, stream=LogStream.standard_output)
    assert text(logs.text) == "remote-card-stdout\n"
    environments = client.call("RunEnvironmentsRequest", runId=run_id).environments
    assert environments and text(environments[0].system) == system
    return summary


def test_remote_card_roundtrip_and_reconnect(project, core_factory, remote_broker, remote_daemon, remote_gates):
    if remote_gates is None:
        pytest.skip("requires a disposable worker whose listener can be restarted")
    fixture(project)
    daemon = core_factory()
    client = daemon.connect()
    services = client.call("ProjectInspectRequest").executionServices
    remote = next(service for service in services if text(service.id) == "remote-test")
    assert remote.available
    system = text(remote.nativeSystem)
    assert system in ("aarch64-linux", "x86_64-linux")

    run_id = start_run(client, "source")
    started = wait_invocation(client, run_id, "slow", lambda row: row.executionState == RemoteExecutionState.running)
    # Once the Card is waiting, restart the listener and release the same
    # invocation. The journal and detached supervisor survive the restart.
    remote_gates(started, before_release=remote_daemon[2])
    run = wait_run(client, run_id)
    assert run.state == RunState.succeeded, run.failure and text(run.failure.diagnostic)
    resumed = check_success(client, run_id, "slow", system)
    assert text(resumed.executionJobId) == text(started.executionJobId)


def test_remote_display_is_live_and_leaves_no_result_file(project, core_factory, remote_results, remote_gates):
    if remote_gates is None:
        pytest.skip("requires the disposable worker's job folders")
    payload = b"remote-opaque-view-353e4f6c\x00\xff\x80"
    source = GATE_HELPER + f"""
from card import Output
import tsugumi

def run(input):
    view = tsugumi.view_open("notes")
    tsugumi.send(view.session, {payload!r})
    wait_for_release()
    tsugumi.view_close(view.session)
    event = tsugumi.recv(10000)
    assert (event.kind, event.session, event.route) == ("close", view.session, view.route)
    try:
        tsugumi.route_register(view.session)
    except tsugumi.WindowClosed:
        pass
    else:
        raise AssertionError("Registered a route on a closed window")
    return Output()
"""
    views = 'views = [{ name = "notes"; type = "html"; blob = ./notes.html; }];'
    definition = {"type": "code", "workerCapabilities": [63],
                  "timeoutSeconds": RUN_LIST_TIMEOUT_SECONDS,
                  "_outputs": "table Outputs {} root_type Outputs;",
                  "_nix": entry(python_run(source), views)}
    write(project, {"work": definition}, [card("work", "work")], [],
          files={"definitions/work/notes.html": "<!doctype html><p>Empty view</p>"})
    client = core_factory().connect(client=Client.Web)
    subscription = text(client.open().subscriptionId)
    run_id = start_run(client, "work")
    active = wait_invocation(client, run_id, "work", lambda row: row.executionState == RemoteExecutionState.running)
    note = open_view(client, timeout=RUN_LIST_TIMEOUT_SECONDS)
    assert note.card.segments == [b"work"]
    assert text(note.name) == "notes" and text(note.type) == "html"
    session = text(note.session)
    assert b"Empty view" in bytes(client.call("CardViewReadRequest", session=session).bytes)
    shown = wait_for_display(client, run_id, lambda update: text(update.session) == session).value.display
    return_credit(client, subscription, shown)
    assert bytes(shown.bytes) == payload
    remote_gates(active)
    client.wait_notification(
        lambda n: n.valueType == NotificationPayload.ViewClosed and text(n.value.session) == session,
        timeout=RUN_LIST_TIMEOUT_SECONDS,
    )
    status = wait_run(client, run_id)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    result = remote_results[text(active.executionJobId)]
    assert set(result) == {"worker.fb", "logs", "environment"}
    assert all(payload not in contents for contents in result.values())


def test_remote_card_cancels_a_running_job(project, core_factory, remote_broker):
    fixture(project)
    client = core_factory().connect()
    run_id = start_run(client, "source")
    running = wait_invocation(client, run_id, "slow", lambda row: row.executionState == RemoteExecutionState.running)
    client.call("RunCancelRequest", runId=run_id)
    assert wait_run(client, run_id).state == RunState.cancelled
    cancelled = client.call("RunRecordRequest", runId=run_id, card=path("slow")).record.summary
    assert cancelled.state == CardState.cancelled and cancelled.executionState == RemoteExecutionState.cancelled
    assert text(cancelled.executionJobId) == text(running.executionJobId)
    assert not client.call("ArtifactListRequest", runId=run_id, card=path("slow")).artifacts


def test_remote_run_list_preserves_implementation_order(project, core_factory, remote_broker, tmp_path):
    def numbers(values, value=""):
        variants = []
        for number in values:
            program = python_run(f"""
from card import Output
from Number import NumberT

def run(input):
    return Output(out=NumberT(value={value}{number}))
""")
            variants.append(f'{{ label = "{number}"; run = {program}; }}')
        return entry("[ " + " ".join(variants) + " ]")

    definitions = {
        "source": {"type": "code", "_outputs": RUN_LIST_OUTPUT, "_nix": numbers([10, 20])},
        "work": {**RUN_LIST_REMOTE, "inputs": [{"name": "in"}],
                 "_nix": numbers([100, 200], 'input.require("in").value + ')},
        "collect": {"type": "collect", "recordIO": "on"},
    }
    write(project, definitions, [card(name, name) for name in definitions],
          [conn("source", "out", "work", "in"), conn("work", "out", "collect", "in")])
    client = core_factory().connect()
    run_id, run = run_and_wait(client, "source")
    assert run.state == RunState.succeeded, run.failure and text(run.failure.diagnostic)

    page = client.call("RunInvocationsRequest", runId=run_id, card=path("work"))
    assert all(row.executionState == RemoteExecutionState.succeeded and row.executionJobId
               for row in page.records)
    assert len({text(row.executionJobId) for row in page.records}) == 4

    assert collected_items(client, tmp_path / "decode", run_id) == [
        {"value": value} for value in (110, 210, 120, 220)]


def test_remote_run_list_failure_cancels_running_sibling(project, core_factory, remote_broker, remote_gates):
    if remote_gates is None:
        pytest.skip("requires a disposable worker to synchronize both jobs")
    variants = []
    for message in ("expected-batch-member-failure", "unexpected-sibling-completion"):
        program = python_run(GATE_HELPER + f"""
import sys

def run(input):
    wait_for_release()
    print({message!r}, file=sys.stderr)
    raise RuntimeError({message!r})
""")
        variants.append(f'{{ label = "{message}"; run = {program}; }}')
    definitions = {
        "fails": {**RUN_LIST_REMOTE, "_nix": entry("[ " + " ".join(variants) + " ]")},
        "collect": {"type": "collect", "recordIO": "on"},
    }
    write(project, definitions, [card(name, name) for name in definitions],
          [conn("fails", "out", "collect", "in")])
    client = core_factory().connect()
    run_id = start_run(client, "fails", concurrency=2)
    running = wait_invocation(client, run_id, "fails", lambda row: row.batchIndex == 2 and
                              row.executionState == RemoteExecutionState.running)
    first = wait_invocation(client, run_id, "fails", lambda row: row.batchIndex == 1 and
                            row.executionState == RemoteExecutionState.running)
    remote_gates(first)
    assert wait_run(client, run_id).state == RunState.failed
    page = client.call("RunInvocationsRequest", runId=run_id, card=path("fails"))
    batch, = page.batches
    assert batch.complete and batch.failed and not batch.outputs
    failed, cancelled = page.records
    assert failed.executionState == RemoteExecutionState.failed
    assert cancelled.executionState == RemoteExecutionState.cancelled
    assert text(cancelled.executionJobId) == text(running.executionJobId)
    log = client.call("LogRangeRequest", runId=run_id, card=path("fails"),
                      invocation=failed.invocation, stream=LogStream.standard_error)
    assert "expected-batch-member-failure" in text(log.text)
    downstream = client.call("RunInvocationsRequest", runId=run_id, card=path("collect"))
    blocked, = downstream.records
    assert blocked.state == CardState.blocked and not downstream.batches


def test_call_timeout_is_separate_from_build_and_transfers(project, core_factory, remote_broker):
    definition = {"type": "code", "workerCapabilities": [63], "timeoutSeconds": 1,
                  "buildTimeoutSeconds": 600, "_outputs": "table Outputs {} root_type Outputs;",
                  "_nix": entry(python_run(GATE_HELPER + "\ndef run(input): wait_for_release()\n"))}
    write(project, {"deadline": definition}, [card("deadline", "deadline")], [])
    client = core_factory().connect()
    run_id, status = run_and_wait(client, "deadline")
    assert status.state == RunState.failed, log(client, run_id, "deadline", 2)
    assert status.failure.kind == FailureKind.timeout


# Card secrets on a remote worker: hostd keeps the value, the core hands it to
# the worker for one running job only, and the worker gives it to the Card's
# process without writing it anywhere.
SENTINEL = "remote-card-secret-sentinel-5e81c0b7d4"
SECRET_PROJECT = "a2b3c4"
USES_SECRET = """
import os
from card import Output
from Number import NumberT

def run(input):
    value = os.environ["database_password"]
    assert value.startswith("remote-card-secret-sentinel-")
    print("remote-card-ran")
    return Output(out=NumberT(value=len(value)))
"""


@pytest.fixture
def secret_project(project):
    """Write the hostd project id before the broker captures it on authorization."""
    (project / ".tsugumi").mkdir(exist_ok=True)
    (project / ".tsugumi/project-id").write_text(SECRET_PROJECT)
    return SECRET_PROJECT


def uses_secret():
    return {**RUN_LIST_REMOTE, "secrets": ["database_password"],
            "_nix": entry(python_run(USES_SECRET))}


def files_containing(root, needle, skip=lambda path: False):
    found = []
    for directory, _, names in os.walk(root):
        for name in names:
            f = Path(directory) / name
            if skip(f) or f.is_symlink() or not f.is_file():
                continue
            if needle in f.read_bytes():
                found.append(str(f))
    return found


def secret_leaks(tmp_path, core, host, derivations=()):
    """Files outside hostd's secret store that hold the value: the worker's
    journal, scratch, blobs and logs, the project, the core's and hostd's own
    files, and the derivation on both sides."""
    needle = SENTINEL.encode()
    worker_store = tmp_path / "daemon-state/store"

    def skip(path):
        # hostd's store holds the value by design; the worker's Nix store is
        # checked through the run's derivation.
        return path.name.startswith("host.sqlite") or worker_store in path.parents

    leaks = files_containing(tmp_path, needle, skip) + files_containing(core.directory, needle)
    if needle in host.log_path.read_bytes():
        leaks.append(str(host.log_path))
    for derivation in derivations:
        for copy in (Path(derivation), worker_store / Path(derivation).relative_to("/")):
            if copy.is_file() and needle in copy.read_bytes():
                leaks.append(str(copy))
    return leaks


def test_remote_card_receives_its_secret_and_no_file_keeps_it(secret_project, project, core_factory,
                                                              remote_broker, remote_daemon, tmp_path):
    if remote_daemon[2] is None:
        pytest.skip("requires a disposable worker whose state folder the test can read")
    host = remote_broker["host"]
    host.call("CardSecretSetRequest", projectId=secret_project, name="database_password", value=SENTINEL)
    write(project, {"use": uses_secret()}, [card("use", "use")], [])
    core = core_factory()
    client = core.connect()
    run_id, status = run_and_wait(client, "use")
    assert status.state == RunState.succeeded, log(client, run_id, "use", 2)
    summary = client.call("RunRecordRequest", runId=run_id, card=path("use")).record.summary
    assert summary.executionState == RemoteExecutionState.succeeded
    assert text(summary.executionService) == "remote-test"
    assert [text(name) for name in summary.secrets] == ["database_password"]
    assert log(client, run_id, "use", LogStream.standard_output) == "remote-card-ran\n"
    derivations = [text(e.derivation) for e in client.call("RunEnvironmentsRequest", runId=run_id).environments]
    assert derivations
    core.stop()
    assert not secret_leaks(tmp_path, core, host, derivations)


def test_missing_remote_secret_fails_before_anything_moves(secret_project, project, core_factory, remote_broker):
    write(project, {"use": uses_secret()}, [card("use", "use")], [])
    client = core_factory().connect()
    run_id, status = run_and_wait(client, "use")
    assert status.state == RunState.failed
    invocation, = client.call("RunInvocationsRequest", runId=run_id, card=path("use")).records
    assert invocation.failure.kind == FailureKind.not_found
    assert "database_password" in text(invocation.failure.diagnostic)
    assert not text(invocation.executionJobId), "nothing was submitted"


@pytest.fixture
def held_secret_delivery(remote_broker, monkeypatch):
    """Forwards the core's broker frames, holding its JobSecretsRequest until
    the test releases it."""
    upstream = remote_broker["endpoint"].removeprefix("unix:")
    arrived, release = threading.Event(), threading.Event()

    class Forward(socketserver.BaseRequestHandler):
        def handle(self):
            with self.request.makefile("rb") as downstream, socket.socket(socket.AF_UNIX) as connection:
                header = R.exact(downstream, 4)
                size, = struct.unpack("<I", header)
                payload = R.exact(downstream, size)
                request = R.MessageT.InitFromPackedBuf(payload).payload.request
                if request.bodyType == R.RequestBody.JobSecretsRequest:
                    arrived.set()
                    release.wait(timeout=600)
                control = request.bodyType == R.RequestBody.CancelRequest
                connection.connect(upstream + ".c" if control else upstream)
                connection.sendall(header + payload)
                with connection.makefile("rb") as response:
                    header = R.exact(response, 4)
                    size, = struct.unpack("<I", header)
                    self.request.sendall(header + R.exact(response, size))

    with tempfile.TemporaryDirectory(prefix="hs-", dir=os.environ.get("TSUGUMI_SOCKET_DIR")) as sockets:
        address = str(Path(sockets) / "broker")
        with socketserver.ThreadingUnixStreamServer(address, Forward) as server:
            server.daemon_threads = True
            os.symlink(address, address + ".c")
            monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER", "unix:" + address)
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                yield arrived, release
            finally:
                release.set()
                server.shutdown()
                thread.join()


def test_remote_worker_restart_before_the_card_starts_fails_the_job(secret_project, project, core_factory,
                                                                   remote_broker, held_secret_delivery,
                                                                   remote_daemon, tmp_path):
    """The values live only in the listener's memory: after a restart the job
    fails, saying why, and nothing sends them again."""
    if remote_daemon[2] is None:
        pytest.skip("requires a disposable worker whose listener can be restarted")
    arrived, release = held_secret_delivery
    host = remote_broker["host"]
    host.call("CardSecretSetRequest", projectId=secret_project, name="database_password", value=SENTINEL)
    write(project, {"use": uses_secret()}, [card("use", "use")], [])
    core = core_factory()
    client = core.connect()
    run_id = start_run(client, "use")
    # The job is built and waits for its secrets; the core is sending them.
    assert arrived.wait(timeout=RUN_LIST_TIMEOUT_SECONDS)
    remote_daemon[2]()
    release.set()
    assert wait_run(client, run_id).state == RunState.failed
    invocation, = client.call("RunInvocationsRequest", runId=run_id, card=path("use")).records
    assert "restarted before the Card started" in text(invocation.failure.diagnostic), \
        text(invocation.failure.diagnostic)
    assert "remote-card-ran" not in log(client, run_id, "use", LogStream.standard_output)
    core.stop()
    assert not secret_leaks(tmp_path, core, host)
