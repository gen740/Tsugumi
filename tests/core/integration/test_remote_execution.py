"""Opt-in execution against a disposable Linux daemon.

Set TSUGUMI_REMOTE_TEST_DAEMON to start a packaged daemon on loopback.
For a daemon already running, set TSUGUMI_REMOTE_TEST_ENDPOINT and
TSUGUMI_REMOTE_TEST_PUBLIC_KEY_FILE, plus TSUGUMI_REMOTE_TEST_AUTHORIZED_KEYS
pointing to that worker's dedicated writable authorized-keys file.
Use tsugumi-test-env and pytest -m environment.
"""

import hashlib
import json
import os
import shutil
import socket
import subprocess
import time
from pathlib import Path

import pytest
from harness import remote_worker as R
from harness.host import Host
from harness.observations import read_output
from harness.values import reference_json
from projects import (
    FILE_FBS,
    SINK,
    card,
    conn,
    run_and_wait,
    start_run,
    wait_run,
    write,
)
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RemoteExecutionState import RemoteExecutionState
from Tsugumi.Data.RunState import RunState
from tsugumi_client import RpcFailure, path, text

pytestmark = pytest.mark.environment

RUN_LIST_OUTPUT = 'table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;'
# Each remote job realizes its dependencies in a private store, even during a batch.
RUN_LIST_TIMEOUT_SECONDS = 1200
RUN_LIST_REMOTE = {
    "language": "bash", "recordIO": True, "executionService": "remote-test",
    "timeoutSeconds": RUN_LIST_TIMEOUT_SECONDS, "_outputs": RUN_LIST_OUTPUT,
}

@pytest.fixture
def remote_daemon(tmp_path, request):
    binary = os.environ.get("TSUGUMI_REMOTE_TEST_DAEMON")
    if not binary:
        if getattr(request, "param", None):
            pytest.skip("worker settings tests require a disposable packaged daemon")
        endpoint = os.environ.get("TSUGUMI_REMOTE_TEST_ENDPOINT")
        key_file = os.environ.get("TSUGUMI_REMOTE_TEST_PUBLIC_KEY_FILE")
        if not endpoint or not key_file:
            pytest.skip("set TSUGUMI_REMOTE_TEST_DAEMON or a dedicated endpoint and public key file")
        yield endpoint, key_file, None
        return

    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    state = tmp_path / "daemon-state"
    state.mkdir(mode=0o700)
    env = {**os.environ, "TSUGUMI_REMOTE_STATE_DIR": str(state)}
    command = [binary, "--host", "127.0.0.1", "--port", str(port),
               *getattr(request, "param", ())]
    key_file = tmp_path / "worker-public-key"
    key_file.write_bytes(subprocess.check_output([binary, "--public-key"], env=env))
    daemon_log = tmp_path / "execution-daemon.log"
    process = None

    def start():
        nonlocal process
        with daemon_log.open("ab") as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
        until = time.monotonic() + 10
        while time.monotonic() < until:
            if process.poll() is not None:
                raise RuntimeError(daemon_log.read_text())
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.05)
        raise TimeoutError("the test execution daemon did not listen")

    def restart():
        process.kill()
        process.wait(timeout=5)
        start()

    try:
        start()
        yield f"tls://127.0.0.1:{port}", str(key_file), restart
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)


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
        entries = host.call("ExecutionServicePutRequest", id="remote-test", name="Remote test",
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
                 "serverId": R.string(entries.services[0].capabilities.serverId)}
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER", ready["endpoint"])
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE", ready["capability"])
        yield ready
    finally:
        host.close()
        if external_authorized is not None and added_key is not None:
            # Remove only this fixture's key, preserving other hosts and edits.
            lines = external_authorized.read_text().splitlines(keepends=True)
            external_authorized.write_text("".join(line for line in lines if line.strip() != added_key))


def broker_request(endpoint, capability_file, server_id, body_type, body, **routing):
    envelope = R.broker_request(Path(capability_file).read_text().strip(), body_type, body,
                                service="remote-test", serverId=server_id, **routing)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(30)
        connection.connect(endpoint.removeprefix("unix:"))
        connection.sendall(R.frame(envelope))
        connection.shutdown(socket.SHUT_WR)
        with connection.makefile("rb") as response:
            return R.read(response).payload


def test_remote_broker_isolates_copies_of_a_project(remote_broker):
    endpoint, original, copy, server_id = (remote_broker[name] for name in ("endpoint", "capability", "copyCapability", "serverId"))
    first = broker_request(endpoint, original, server_id, R.RequestBody.ProbeRequest, R.ProbeRequestT()).body
    second = broker_request(endpoint, copy, server_id, R.RequestBody.ProbeRequest, R.ProbeRequestT()).body
    assert first.instanceId != second.instanceId
    value = b"private to the original project\n"
    digest = hashlib.sha256(value).hexdigest()
    job = "8" * 64
    uploaded = broker_request(endpoint, original, server_id, R.RequestBody.PutBlobRequest,
        R.PutBlobRequestT(digest=digest, total=len(value), bytes=value), instanceId=first.instanceId, jobId=job)
    assert uploaded.bodyType == R.ReplyBody.Ack
    accepted = broker_request(endpoint, original, server_id, R.RequestBody.GetBlobRequest,
        R.GetBlobRequestT(digest=digest), instanceId=first.instanceId, jobId=job)
    assert accepted.bodyType == R.ReplyBody.BlobChunk and bytes(accepted.body.bytes) == value
    for instance in (first.instanceId, second.instanceId):
        refused = broker_request(endpoint, copy, server_id, R.RequestBody.GetBlobRequest,
            R.GetBlobRequestT(digest=digest), instanceId=instance, jobId=job)
        assert refused.bodyType == R.ReplyBody.Failure


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
        "work": {**settings, "_nix": "{ pkgs, inputs, params }: { run = ''" + script + "''; }"},
        "slow": {**settings, "_nix": "{ pkgs, inputs, params }: { run = ''sleep 25\n" + script + "''; }"},
        "sink": SINK,
    }
    write(project, definitions,
          [card("source", "input"), card("later", "input"), card("work", "work"), card("slow", "slow"),
           card("after", "sink")],
          [conn("source", "doc", "work", "in"), conn("later", "doc", "slow", "in"),
           conn("slow", "doc", "after", "in")],
          {"definitions/input/payload.txt": "remote-input-roundtrip\n"})


def wait_invocation(client, run_id, name, check, timeout=480):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try:
            values = client.call("RunInvocationsRequest", runId=run_id, card=path(name)).records or []
            if values and check(values[-1]):
                return values[-1]
        except RpcFailure as error:
            if error.kind != "not_found":
                raise
        time.sleep(0.5)
    raise TimeoutError(f"external Card {name} did not reach the expected invocation state")


def check_success(client, run_id, name, system):
    record = client.call("RunRecordRequest", runId=run_id, card=path(name)).record
    summary = record.summary
    assert summary.state == CardState.succeeded, summary.failure and text(summary.failure.diagnostic)
    assert summary.executionState == RemoteExecutionState.succeeded
    assert text(summary.executionService) == "remote-test"
    assert text(summary.executionSystem) == system
    assert len(text(summary.executionJobId)) == 64
    assert not summary.executionResources
    assert text(record.display.value.text) == "remote Card display"
    output, = record.outputs
    assert text(output.rootType) == "files.Doc"
    artifacts = client.call("ArtifactListRequest", runId=run_id, card=path(name)).artifacts
    assert len(artifacts) == 1
    content = client.call("ArtifactReadRequest", artifactId=text(artifacts[0].artifactId), limitBytes=1024)
    assert bytes(content.bytes) == b"remote-input-roundtrip\n"
    member, = artifacts[0].members
    assert text(member.hash) == hashlib.sha256(bytes(content.bytes)).hexdigest()
    logs = client.call("LogRangeRequest", runId=run_id, card=path(name), invocation=1, stream=LogStream.stdout)
    assert text(logs.text) == "remote-card-stdout\n"
    environments = client.call("RunEnvironmentsRequest", runId=run_id).environments
    assert environments and text(environments[0].system) == system
    return summary


def test_remote_card_roundtrip_and_reconnect(project, core_factory, remote_broker, remote_daemon):
    fixture(project)
    daemon = core_factory()
    client = daemon.connect()
    services = client.call("ProjectInspectRequest").executionServices
    remote = next(service for service in services if text(service.id) == "remote-test")
    assert remote.available
    system = text(remote.nativeSystem)
    assert system in ("aarch64-linux", "x86_64-linux")

    run_id = start_run(client, "source")
    run = wait_run(client, run_id, timeout=480)
    assert run.state == RunState.succeeded, run.failure and text(run.failure.diagnostic)
    check_success(client, run_id, "work", system)

    run_id = start_run(client, "later")
    started = wait_invocation(client, run_id, "slow", lambda row: row.executionState == RemoteExecutionState.running)
    if restart := remote_daemon[2]:
        restart()
    daemon.restart(crash=True)
    client = daemon.connect()
    wait_invocation(client, run_id, "slow", lambda row: row.endedAt is not None)
    recovered = check_success(client, run_id, "slow", system)
    assert text(recovered.executionJobId) == text(started.executionJobId)
    assert client.call("RunStatusRequest", runId=run_id).state == RunState.failed
    with pytest.raises(RpcFailure) as downstream:
        client.call("RunRecordRequest", runId=run_id, card=path("after"))
    assert downstream.value.kind == "not_found"


@pytest.mark.parametrize("remote_daemon", [("--log-limit-bytes", "1")], indirect=True)
def test_remote_card_reports_log_truncation(project, core_factory, remote_broker):
    fixture(project)
    client = core_factory().connect()
    run_id = start_run(client, "source")
    run = wait_run(client, run_id, timeout=480)
    assert run.state == RunState.succeeded, run.failure and text(run.failure.diagnostic)
    stdout = client.call("LogRangeRequest", runId=run_id, card=path("work"),
                         invocation=1, stream=LogStream.stdout)
    assert not text(stdout.text)
    journal = client.call("LogRangeRequest", runId=run_id, card=path("work"),
                          invocation=1, stream=LogStream.journal)
    assert text(journal.text).count("[Log output truncated at the 1-byte limit.]") == 1


def test_remote_card_cancels_a_running_job(project, core_factory, remote_broker):
    fixture(project)
    client = core_factory().connect()
    run_id = start_run(client, "later")
    running = wait_invocation(client, run_id, "slow", lambda row: row.executionState == RemoteExecutionState.running)
    client.call("RunCancelRequest", runId=run_id)
    assert wait_run(client, run_id, timeout=60).state == RunState.cancelled
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
    run_id, run = run_and_wait(client, "source", timeout=RUN_LIST_TIMEOUT_SECONDS + 60)
    assert run.state == RunState.succeeded, run.failure and text(run.failure.diagnostic)

    page = client.call("RunInvocationsRequest", runId=run_id, card=path("work"))
    batch, = page.batches
    assert batch.complete and not batch.failed and batch.executionList
    assert batch.activation == 1 and batch.invocationCount == 4
    assert [(row.batchActivation, row.batchIndex, row.batchSize, row.implementationIndex)
            for row in page.records] == [(1, 1, 4, 1), (1, 2, 4, 2), (1, 3, 4, 1), (1, 4, 4, 2)]
    for row in page.records:
        assert row.executionState == RemoteExecutionState.succeeded
        assert text(row.executionService) == "remote-test"
        assert row.executionResources is None
        assert len(text(row.executionJobId)) == 64
    assert len({text(row.executionJobId) for row in page.records}) == 4

    record = client.call("RunRecordRequest", runId=run_id, card=path("collect")).record
    output, = record.outputs
    payload = read_output(client, run_id, card="collect")
    collected = reference_json(tmp_path / "decode", bytes(output.schemaBfbs), payload,
                               root_type=text(output.rootType))
    assert collected["items"] == [{"value": value} for value in (110, 210, 120, 220)]


def test_remote_run_list_failure_cancels_running_sibling(project, core_factory, remote_broker):
    definitions = {
        "fails": {
            **RUN_LIST_REMOTE,
            "_nix": """
                { pkgs, inputs, params }: {
                  run = [
                    "sleep 15; echo expected-batch-member-failure >&2; exit 17"
                    "sleep 180; exit 99"
                  ];
                }
            """,
        },
        "collect": {"template": "collect", "recordIO": True},
    }
    write(project, definitions, [card(name, name) for name in definitions],
          [conn("fails", "out", "collect", "in")])
    client = core_factory().connect()
    run_id = start_run(client, "fails", concurrency=2)
    running = wait_invocation(client, run_id, "fails", lambda row: row.batchIndex == 2 and
                              row.executionState == RemoteExecutionState.running)
    assert wait_run(client, run_id, timeout=RUN_LIST_TIMEOUT_SECONDS + 60).state == RunState.failed
    page = client.call("RunInvocationsRequest", runId=run_id, card=path("fails"))
    batch, = page.batches
    assert batch.complete and batch.failed and not batch.outputs
    failed, cancelled = page.records
    assert failed.executionState == RemoteExecutionState.failed
    assert cancelled.executionState == RemoteExecutionState.cancelled
    assert text(cancelled.executionJobId) == text(running.executionJobId)
    log = client.call("LogRangeRequest", runId=run_id, card=path("fails"),
                      invocation=failed.invocation, stream=LogStream.stderr)
    assert "expected-batch-member-failure" in text(log.text)
    downstream = client.call("RunInvocationsRequest", runId=run_id, card=path("collect"))
    blocked, = downstream.records
    assert blocked.state == CardState.blocked and not downstream.batches


@pytest.mark.parametrize("directory", [False, True])
def test_remote_transfers_only_declared_outputs(project, core_factory, remote_broker, tmp_path, directory):
    """Remote execution ignores undeclared files/FIFOs and materializes relative links within a declared Directory."""
    from harness.real_cards import bash, log
    schema = FILE_FBS.replace('tsugumi: "file"', 'tsugumi: "directory"') if directory else FILE_FBS
    create = ('mkdir "$TSUGUMI_OUTPUT_DIR/report"; '
              'echo safe > "$TSUGUMI_OUTPUT_DIR/report/data"; '
              'ln -s data "$TSUGUMI_OUTPUT_DIR/report/link"; '
              'result=report; ' if directory else
              'echo safe > "$TSUGUMI_OUTPUT_DIR/result"; result=result; ')
    script = (create +
              'mkfifo "$TSUGUMI_OUTPUT_DIR/undeclared-fifo"; '
              'echo unused > "$TSUGUMI_OUTPUT_DIR/undeclared-file"; '
              'mkfifo "$TSUGUMI_WORK_DIR/undeclared-fifo"; '
              'mkfifo "$TSUGUMI_TMP_DIR/undeclared-fifo"; '
              'printf \'{"doc":{"value":"%s/%s"}}\' "$TSUGUMI_OUTPUT_DIR" "$result" > "$TSUGUMI_OUTPUT"')
    definition = bash(script, outputs=schema, executionService="remote-test", recordIO=True,
                      filesystem=True, timeoutSeconds=600)
    write(project, {"copy": definition}, [card("copy", "copy")], [])
    scratch = tmp_path / "scratch"
    c = core_factory(args=["--run-scratch-dir", str(scratch)]).connect()
    run_id, st = run_and_wait(c, "copy", timeout=900)
    assert st.state == RunState.succeeded, log(c, run_id, "copy", 2)
    record = c.call("RunRecordRequest", runId=run_id, card=path("copy")).record
    saved = Path(text(record.outputs[0].filePath))
    if directory:
        assert (saved / "data").read_text() == "safe\n"
        assert (saved / "link").read_text() == "safe\n"
        assert not (saved / "link").is_symlink()
    else:
        assert saved.read_text() == "safe\n"
    invocation = scratch / "runs" / run_id / "work/copy/1"
    assert not list(invocation.rglob("undeclared-*"))
