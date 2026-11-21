"""Opt-in execution against a disposable Linux tsugumi-workerd.

Set TSUGUMI_REMOTE_TEST_DAEMON to start packaged tsugumi-workerd on loopback.
For a daemon already running, set TSUGUMI_REMOTE_TEST_ENDPOINT and
TSUGUMI_REMOTE_TEST_PUBLIC_KEY_FILE, plus TSUGUMI_REMOTE_TEST_AUTHORIZED_KEYS
pointing to that worker's dedicated writable authorized-keys file.
Use tsugumi-test-env and pytest -m environment.
"""

import hashlib
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
from pathlib import Path

import pytest
from harness import remote_worker as R
from harness.file_events import process_file, wait_for_file
from harness.host import Host
from harness.observations import read_output, read_pages
from harness.values import reference_json
from projects import (
    FILE_FBS,
    card,
    conn,
    run_and_wait,
    start_run,
    wait_invocation,
    wait_run,
    write,
)
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RemoteExecutionState import RemoteExecutionState
from Tsugumi.Data.RunState import RunState
from tsugumi_client import path, text

pytestmark = pytest.mark.environment

RUN_LIST_OUTPUT = 'table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;'
# Each remote job realizes its dependencies in a private store, even during a batch.
RUN_LIST_TIMEOUT_SECONDS = 1200
RUN_LIST_REMOTE = {
    "language": "bash", "recordIO": True, "executionService": "remote-test",
    "timeoutSeconds": RUN_LIST_TIMEOUT_SECONDS, "_outputs": RUN_LIST_OUTPUT,
}

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
        ready = state / "worker.ready.json"
        ready.unlink(missing_ok=True)
        with daemon_log.open("ab") as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        published = process_file(process, ready)
        if published is None:
            raise RuntimeError(daemon_log.read_text())
        port = json.loads(published.read_text())["port"]
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
                  endpoint=endpoint, workerPublicKey=Path(key_file).read_text().strip(), verify=True)

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
def module_downloads(remote_broker, monkeypatch):
    """Observe blob requests while forwarding the broker's original frames."""
    upstream = remote_broker["endpoint"].removeprefix("unix:")
    downloads = set()

    class Forward(socketserver.BaseRequestHandler):
        def handle(self):
            with self.request.makefile("rb") as downstream, socket.socket(socket.AF_UNIX) as connection:
                header = R.exact(downstream, 4)
                size, = struct.unpack("<I", header)
                payload = R.exact(downstream, size)
                request = R.MessageT.InitFromPackedBuf(payload).payload.request
                if request.bodyType == R.RequestBody.GetBlobRequest:
                    downloads.add(text(request.body.digest))
                control = request.bodyType == R.RequestBody.CancelRequest
                connection.connect(upstream + ".c" if control else upstream)
                connection.sendall(header + payload)
                with connection.makefile("rb") as response:
                    header = R.exact(response, 4)
                    size, = struct.unpack("<I", header)
                    self.request.sendall(header + R.exact(response, size))

    with tempfile.TemporaryDirectory(prefix="mb-", dir=os.environ.get("TSUGUMI_SOCKET_DIR")) as sockets:
        address = str(Path(sockets) / "broker")
        with socketserver.ThreadingUnixStreamServer(address, Forward) as server:
            os.symlink(address, address + ".c")
            monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER", "unix:" + address)
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                yield downloads
            finally:
                server.shutdown()
                thread.join()


def test_remote_broker_isolates_copies_of_a_project(remote_broker):
    endpoint, original, copy = (remote_broker[name] for name in ("endpoint", "capability", "copyCapability"))
    # Keep the life ID identical to check project scoping independently of job ownership.
    core_life = secrets.token_hex(32)
    first = broker_request(endpoint, original, R.RequestBody.ProbeRequest, R.ProbeRequestT(), coreLifeId=core_life)
    second = broker_request(endpoint, copy, R.RequestBody.ProbeRequest, R.ProbeRequestT(), coreLifeId=core_life)
    assert first.bodyType == R.ReplyBody.Capabilities, vars(first.body)
    assert second.bodyType == R.ReplyBody.Capabilities, vars(second.body)
    first, second = first.body, second.body
    assert first.instanceId != second.instanceId
    value = b"private to the original project\n"
    digest = hashlib.sha256(value).hexdigest()
    job = "8" * 64
    uploaded = broker_request(endpoint, original, R.RequestBody.PutBlobRequest,
        R.PutBlobRequestT(digest=digest, total=len(value), bytes=value), instanceId=first.instanceId, jobId=job, coreLifeId=core_life)
    assert uploaded.bodyType == R.ReplyBody.Ack
    accepted = broker_request(endpoint, original, R.RequestBody.GetBlobRequest,
        R.GetBlobRequestT(digest=digest), instanceId=first.instanceId, jobId=job, coreLifeId=core_life)
    assert accepted.bodyType == R.ReplyBody.BlobChunk and bytes(accepted.body.bytes) == value
    for instance, code in ((first.instanceId, R.ErrorCode.InstanceChanged),
                           (second.instanceId, R.ErrorCode.NotFound)):
        refused = broker_request(endpoint, copy, R.RequestBody.GetBlobRequest,
            R.GetBlobRequestT(digest=digest), instanceId=instance, jobId=job, coreLifeId=core_life)
        assert refused.bodyType == R.ReplyBody.Failure
        assert refused.body.code == code, R.string(refused.body.message)


# HOME is the writable work folder inside this invocation's bound scratch.
CARD_GATE = 'mkfifo -m 666 "$HOME/test-release" && read -r release < "$HOME/test-release"'

@pytest.fixture
def remote_gates(tmp_path, remote_daemon):
    if remote_daemon[2] is None:
        # The externally managed worker exposes status but has no fixture
        # channel for releasing a staged Card inside its sandbox.
        return None

    def release(invocation):
        job = text(invocation.executionJobId)
        directory, = (tmp_path / "daemon-state/projects").glob(f"*/jobs/{job}")
        fifo = wait_for_file(directory / "work/scratch/work/test-release")
        with fifo.open("w") as gate:
            gate.write("release\n")

    return release


def fixture(project):
    script = """
      set -e
      test "$(uname -s)" = Linux
      test ! -e /etc/hostname
      test -z "$TSUGUMI_REMOTE_PRIVATE_KEY"
      printf 'remote-card-stdout\\n'
      printf '%s' '{"text":"remote Card display"}' > "$TSUGUMI_REPORT"
      cp "$FILE_IN" "$TSUGUMI_OUTPUT_DIR/copied.txt"
      jq -nc --arg f "$TSUGUMI_OUTPUT_DIR/copied.txt" '{doc:{value:$f}}' > "$TSUGUMI_OUTPUT"
    """
    source = {"template": "input", "params": [{"name": "value", "type": "string", "data": "payload.txt"}],
              "_outputs": FILE_FBS}
    settings = {
        "language": "bash", "inputs": [{"name": "in"}], "_outputs": FILE_FBS,
        "executionService": "remote-test",
        "retainArtifact": True, "artifactOutput": "doc", "recordIO": True, "timeoutSeconds": 600,
    }
    definitions = {
        "input": source,
        "slow": {**settings, "_nix": "{ pkgs, inputs, params }: { run = ''" + CARD_GATE + "\n" + script + "''; }"},
    }
    write(project, definitions, [card("source", "input"), card("slow", "slow")],
          [conn("source", "doc", "slow", "in")],
          {"definitions/input/payload.txt": "remote-input-roundtrip\n"})


def check_success(client, run_id, name, system):
    record = client.call("RunRecordRequest", runId=run_id, card=path(name)).record
    summary = record.summary
    assert summary.state == CardState.succeeded, summary.failure and text(summary.failure.diagnostic)
    assert summary.executionState == RemoteExecutionState.succeeded
    assert text(summary.executionService) == "remote-test"
    assert text(summary.executionSystem) == system
    assert text(record.display.value.text) == "remote Card display"
    artifacts = client.call("ArtifactListRequest", runId=run_id, card=path(name)).artifacts
    assert len(artifacts) == 1
    content = client.call("ArtifactReadRequest", artifactId=text(artifacts[0].artifactId), limitBytes=1024)
    assert bytes(content.bytes) == b"remote-input-roundtrip\n"
    member, = artifacts[0].members
    assert text(member.hash) == hashlib.sha256(bytes(content.bytes)).hexdigest()
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
    # The listener restarts while its journal and detached supervisor survive.
    remote_daemon[2]()
    remote_gates(started)
    run = wait_run(client, run_id)
    assert run.state == RunState.succeeded, run.failure and text(run.failure.diagnostic)
    resumed = check_success(client, run_id, "slow", system)
    assert text(resumed.executionJobId) == text(started.executionJobId)


@pytest.mark.parametrize("view", ["own-js", "own-wasm"])
def test_remote_display_serves_card_owned_modules(project, core_factory, module_downloads, monkeypatch, view):
    marker = "remote-module-" + hashlib.sha256(str(project).encode()).hexdigest()[:16]
    config = {"answer": 42}
    blob = b"remote view data"
    report = json.dumps({"module": {"module": "own", "config": config, "blobs": {"data": "data.bin"}}})
    script = """
      set -e
      printf '%s' 'remote view data' > data.bin
      printf '%s' '__DISPLAY_REPORT__' > "$TSUGUMI_REPORT"
      printf '%s' '{}' > "$TSUGUMI_OUTPUT"
    """.replace("__DISPLAY_REPORT__", report)
    entry = f'export function initialise(root) {{ root.textContent = "{marker}"; }}\n'
    if view == "own-js":
        views = f'views.own = inputs.tsugumi.viewModule {{ src = ./view; name = "{marker}-js"; }};'
    else:
        # Exercise the SDK's shared-index.js / Card-owned view.wasm layout
        # without building the much larger WebGPU template for this RPC test.
        entry = (f'// {marker}\nexport function initialise(root) {{ '
                 'root.textContent = new WebAssembly.Instance(new WebAssembly.Module(tsugumiWasm)).exports.answer(); }\n')
        views = f"""
          views.own = pkgs.runCommand "{marker}-wasm" {{ nativeBuildInputs = [ pkgs.wabt ]; }} ''
            mkdir -p $out
            wat2wasm ${{./view/view.wat}} -o $out/view.wasm
            ln -s ${{pkgs.runCommand "{marker}-template" {{ }} ''
              mkdir -p $out
              cp ${{./view/index.js}} $out/index.js
            ''}}/index.js $out/index.js
          '';
        """
    entry += 'export function update(){}\nexport function resize(){}\nexport function dispose(){}\n'
    definition = {"language": "bash", "timeoutSeconds": RUN_LIST_TIMEOUT_SECONDS,
                  "_outputs": "table Outputs {} root_type Outputs;",
                  "_nix": "{ pkgs, inputs, params }: { " + views + " run = ''" + script + "''; }"}
    write(project, {"display": definition},
          [card("remote", "display", executionService="remote-test")], [],
          {"definitions/display/view/index.js": entry,
           "definitions/display/view/view.wat": '(module (func (export "answer") (result i32) i32.const 42))'})
    daemon = core_factory()
    client = daemon.connect()

    def run_display():
        run_id, status = run_and_wait(client, "remote")
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        record = client.call("RunRecordRequest", runId=run_id, card=path("remote")).record
        assert record.summary.executionState == RemoteExecutionState.succeeded
        return run_id, record.display.value

    remote_run, remote = run_display()
    identity = text(remote.module)
    assert not Path(identity).exists(), "the Card-owned output must only exist on the worker"
    assert json.loads(text(remote.configJson)) == config
    assert {text(b.name): bytes(b.data) for b in remote.blobs} == {"data": blob}
    remote_source = read_pages(client, "ModuleReadRequest", module=identity, page_bytes=65536)
    node = shutil.which("node")
    if not node:
        pytest.skip("needs Node.js to exercise the retrieved module")
    script = """
import fs from 'node:fs';
const code = fs.readFileSync(0);
const module = await import('data:text/javascript;base64,' + code.toString('base64'));
const root = {};
await module.initialise(root);
process.stdout.write(JSON.stringify(root.textContent));
"""
    result = subprocess.check_output([node, "--input-type=module", "-e", script], input=remote_source)
    assert json.loads(result) == (42 if view == "own-wasm" else marker)
    if view == "own-js":
        digest = hashlib.sha256(remote_source).hexdigest()
        assert digest in module_downloads
        module_downloads.clear()
        _, repeated = run_display()
        assert text(repeated.module) == identity
        assert read_pages(client, "ModuleReadRequest", module=identity, page_bytes=65536) == remote_source
        assert digest not in module_downloads
    else:
        # Serve the recorded module after a restart with no worker access.
        with monkeypatch.context() as offline:
            offline.delenv("TSUGUMI_EXECUTION_BROKER")
            offline.delenv("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE")
            daemon.restart()
            client = daemon.connect()
            recorded = client.call("RunRecordRequest", runId=remote_run, card=path("remote")).record.display.value
            assert text(recorded.module) == identity
            assert read_pages(client, "ModuleReadRequest", module=identity, page_bytes=65536) == remote_source


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
    definitions = {
        "source": {
            "language": "bash", "_outputs": RUN_LIST_OUTPUT,
            "_nix": """
                { pkgs, inputs, params }: {
                  run = [
                    ''printf '%s' '{"out":{"value":10}}' > "$TSUGUMI_OUTPUT"''
                    ''printf '%s' '{"out":{"value":20}}' > "$TSUGUMI_OUTPUT"''
                  ];
                }
            """,
        },
        "work": {
            **RUN_LIST_REMOTE, "inputs": [{"name": "in"}],
            "_nix": """
                { pkgs, inputs, params }: {
                  run = [
                    ''printf '{"out":{"value":%s}}' "$(( $(jq -r .value "$JSON_IN") + 100 ))" > "$TSUGUMI_OUTPUT"''
                    ''printf '{"out":{"value":%s}}' "$(( $(jq -r .value "$JSON_IN") + 200 ))" > "$TSUGUMI_OUTPUT"''
                  ];
                }
            """,
        },
        "collect": {"template": "collect", "recordIO": True},
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

    record = client.call("RunRecordRequest", runId=run_id, card=path("collect")).record
    output, = record.outputs
    payload = read_output(client, run_id, card="collect")
    collected = reference_json(tmp_path / "decode", bytes(output.schemaBfbs), payload,
                               root_type=text(output.rootType))
    assert collected["items"] == [{"value": value} for value in (110, 210, 120, 220)]


def test_remote_run_list_failure_cancels_running_sibling(project, core_factory, remote_broker, remote_gates):
    if remote_gates is None:
        pytest.skip("requires a disposable worker to synchronize both jobs")
    definitions = {
        "fails": {
            **RUN_LIST_REMOTE,
            "_nix": """
                { pkgs, inputs, params }: {
                  run = [
                    ''@fail_gate@; echo expected-batch-member-failure >&2; exit 17''
                    ''@sibling_gate@; exit 99''
                  ];
                }
            """.replace("@fail_gate@", CARD_GATE)
               .replace("@sibling_gate@", CARD_GATE),
        },
        "collect": {"template": "collect", "recordIO": True},
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


def test_remote_transfers_only_declared_outputs(project, core_factory, remote_broker, tmp_path):
    """Remote execution ignores undeclared files/FIFOs and materializes relative links within a declared Directory."""
    from harness.real_cards import bash, log
    schema = FILE_FBS.replace('tsugumi: "file"', 'tsugumi: "directory"')
    create = ('mkdir "$TSUGUMI_OUTPUT_DIR/report"; '
              'echo safe > "$TSUGUMI_OUTPUT_DIR/report/data"; '
              'ln -s data "$TSUGUMI_OUTPUT_DIR/report/link"; '
              'result=report; ')
    script = (create +
              'mkfifo "$TSUGUMI_OUTPUT_DIR/undeclared-fifo"; '
              'echo unused > "$TSUGUMI_OUTPUT_DIR/undeclared-file"; '
              'mkfifo "$TSUGUMI_WORK_DIR/undeclared-fifo"; '
              'mkfifo "$TSUGUMI_TMP_DIR/undeclared-fifo"; '
              'printf \'{"doc":{"value":"%s/%s"}}\' "$TSUGUMI_OUTPUT_DIR" "$result" > "$TSUGUMI_OUTPUT"')
    definition = bash(script, outputs=schema, executionService="remote-test", recordIO=True,
                      timeoutSeconds=600)
    write(project, {"copy": definition}, [card("copy", "copy")], [])
    scratch = tmp_path / "scratch"
    c = core_factory(args=["--run-scratch-dir", str(scratch)]).connect()
    run_id, st = run_and_wait(c, "copy")
    assert st.state == RunState.succeeded, log(c, run_id, "copy", 2)
    record = c.call("RunRecordRequest", runId=run_id, card=path("copy")).record
    saved = Path(text(record.outputs[0].filePath))
    assert not (saved / "link").is_symlink()
    assert (saved / "link").read_text() == "safe\n"
    invocation = scratch / "runs" / run_id / "work/copy/1"
    assert not list(invocation.rglob("undeclared-*"))


def test_remote_tools_come_from_the_definition_and_are_recorded(project, core_factory, remote_broker):
    """Remote bodies and hooks use the same definition-owned tools and inventory as local runs."""
    from harness.real_cards import DEFINITION_TOOLS_NIX, TXT, check_definition_tools

    write(project, {"work": {"language": "bash", "recordIO": True, "executionService": "remote-test",
                             "timeoutSeconds": 600, "_outputs": TXT, "_nix": DEFINITION_TOOLS_NIX}},
          [card("work", "work")], [])
    client = core_factory().connect()
    run_id, status = run_and_wait(client, "work")
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    check_definition_tools(client, run_id)


def test_call_timeout_is_separate_from_build_and_transfers(project, core_factory, remote_broker):
    from harness.real_cards import bash, log
    definition = bash(CARD_GATE, executionService="remote-test", timeoutSeconds=1, buildTimeoutSeconds=600)
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
USES_SECRET = (
    'case "$database_password" in ' + SENTINEL.rsplit("-", 1)[0] + '-*) ;; *) exit 3 ;; esac; '
    f'test "${{#database_password}}" = {len(SENTINEL)} || exit 5; '
    'test "$FROM_PRE" = ready || exit 4; '
    'echo remote-card-ran; '
    'printf \'{"out":{"value":%d}}\' "${#database_password}" > "$TSUGUMI_OUTPUT"'
)


@pytest.fixture
def secret_project(project):
    """The hostd project id the broker binds this project's Card secrets to."""
    (project / ".tsugumi").mkdir(exist_ok=True)
    (project / ".tsugumi/project-id").write_text(SECRET_PROJECT)
    return SECRET_PROJECT


def uses_secret():
    from harness.real_cards import bash
    # preRun's environment, the secret included, comes back to the worker.
    return bash(USES_SECRET, pre="export FROM_PRE=ready", secrets=["database_password"],
                recordIO=True, executionService="remote-test", timeoutSeconds=600)


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
    from harness.real_cards import log
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
    from harness.real_cards import log
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
    assert "restarted" in text(invocation.failure.diagnostic), text(invocation.failure.diagnostic)
    assert "remote-card-ran" not in log(client, run_id, "use", LogStream.standard_output)
    core.stop()
    assert not secret_leaks(tmp_path, core, host)
