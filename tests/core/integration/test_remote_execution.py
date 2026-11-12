"""Opt-in execution against a disposable Linux daemon.

Set TSUGUMI_REMOTE_TEST_DAEMON to start a packaged daemon on loopback.
For a daemon already running, set TSUGUMI_REMOTE_TEST_ENDPOINT and
TSUGUMI_REMOTE_TEST_TOKEN_FILE. Use tsugumi-test-env and pytest -m environment.
"""

import hashlib
import json
import os
import select
import secrets
import shutil
import socket
import struct
import subprocess
import time
from pathlib import Path

import pytest
from harness.observations import read_output
from harness.values import reference_json
from projects import FILE_FBS, SINK, card, conn, run_and_wait, start_run, wait_run, write
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

BROKER = """
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
const [modulePath, support, project, copy, endpoint, tokenFile] = process.argv.slice(2);
const { authorizeExecutionProject, registerExecutionService, startExecutionBroker } =
  await import(pathToFileURL(modulePath));
const authorize = (project, name) => {
  const control = path.join(support, name);
  fs.mkdirSync(control, {recursive: true, mode: 0o700});
  const st = fs.statSync(project, {bigint: true});
  return authorizeExecutionProject(support, [st.dev, st.ino, st.birthtimeNs || st.ctimeNs].join(":"), control);
};
const token = authorize(project, "control");
const copyToken = authorize(copy, "copy-control");
const entries = await registerExecutionService(support, {id: "remote-test", name: "Remote test", endpoint,
                                                         token: fs.readFileSync(tokenFile, "utf8").trim()});
const broker = await startExecutionBroker(support);
console.log(JSON.stringify({endpoint: broker, token, copyToken, serverId: entries[0].capabilities.serverId}));
setInterval(() => {}, 30000);
"""


@pytest.fixture
def remote_daemon(tmp_path):
    binary = os.environ.get("TSUGUMI_REMOTE_TEST_DAEMON")
    if not binary:
        endpoint = os.environ.get("TSUGUMI_REMOTE_TEST_ENDPOINT")
        token_file = os.environ.get("TSUGUMI_REMOTE_TEST_TOKEN_FILE")
        if not endpoint or not token_file:
            pytest.skip("set TSUGUMI_REMOTE_TEST_DAEMON or a dedicated endpoint and token file")
        yield endpoint, token_file, None
        return

    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    token_file = tmp_path / "daemon-token"
    with os.fdopen(os.open(token_file, os.O_CREAT | os.O_WRONLY | os.O_EXCL, 0o600), "w") as token:
        token.write(secrets.token_hex(32) + "\n")
    env = {**os.environ, "TSUGUMI_REMOTE_STATE_DIR": str(tmp_path / "daemon-state")}
    command = [binary, "--host", "127.0.0.1", "--port", str(port), "--token-file", str(token_file)]
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
        yield f"http://127.0.0.1:{port}", str(token_file), restart
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
    endpoint, token_file, _ = remote_daemon
    script = tmp_path / "broker.mjs"
    script.write_text(BROKER)
    copy = tmp_path / "copied-project"
    shutil.copytree(project, copy)
    module = Path(__file__).resolve().parents[3] / "web/server/executionServices.ts"
    process = subprocess.Popen(["node", str(script), str(module), str(tmp_path / "support"),
                                str(project), str(copy), endpoint, token_file], stdout=subprocess.PIPE, text=True)
    try:
        ready, _, _ = select.select([process.stdout], [], [], 30)
        assert ready, "execution broker did not start"
        line = process.stdout.readline()
        assert line, "execution broker exited before starting"
        ready = json.loads(line)
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER", ready["endpoint"])
        monkeypatch.setenv("TSUGUMI_EXECUTION_BROKER_TOKEN_FILE", ready["token"])
        yield ready
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def broker_request(endpoint, token_file, server_id, operation, body=b"", **fields):
    request = json.dumps({"version": 1, "operation": operation, "service": "remote-test",
                          "broker_token": Path(token_file).read_text().strip(), "server_id": server_id, **fields}).encode()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(30)
        connection.connect(endpoint.removeprefix("unix:"))
        connection.sendall(struct.pack(">I", len(request)) + request + body)
        connection.shutdown(socket.SHUT_WR)
        with connection.makefile("rb") as response:
            size, = struct.unpack(">I", response.read(4))
            return json.loads(response.read(size)), response.read()


def test_remote_broker_isolates_copies_of_a_project(remote_broker):
    endpoint, original, copy, server_id = (remote_broker[name] for name in ("endpoint", "token", "copyToken", "serverId"))
    first, _ = broker_request(endpoint, original, server_id, "probe")
    second, _ = broker_request(endpoint, copy, server_id, "probe")
    assert first["ok"] and second["ok"] and first["instance_id"] != second["instance_id"]

    value = b"private to the original project\n"
    digest = hashlib.sha256(value).hexdigest()
    uploaded, _ = broker_request(endpoint, original, server_id, "put_blob", value, instance_id=first["instance_id"],
                                 sha256=digest, size=len(value))
    assert uploaded["ok"]
    accepted, body = broker_request(endpoint, original, server_id, "get_blob", instance_id=first["instance_id"], sha256=digest)
    assert accepted["ok"] and body == value
    for instance in (first["instance_id"], second["instance_id"]):
        refused, body = broker_request(endpoint, copy, server_id, "get_blob", instance_id=instance, sha256=digest)
        assert not refused["ok"] and not body


def fixture(project):
    script = """
      set -e
      test "$(uname -s)" = Linux
      test ! -e /etc/hostname
      test -z "$TSUGUMI_REMOTE_TOKEN"
      printf 'remote-card-stdout\\n'
      printf '%s' '{"text":"remote Card display"}' > "$TSUGUMI_DISPLAY"
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
    logs = client.call("LogRangeRequest", runId=run_id, card=path(name), invocation=1, stream=LogStream.stdout)
    assert text(logs.text) == "remote-card-stdout\n"
    environments = client.call("RunEnvironmentsRequest", runId=run_id).environments
    assert environments and text(environments[0].system) == system
    return summary


def test_remote_card_roundtrip_reconnect_and_cancel(project, core_factory, remote_broker, remote_daemon):
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
    def running(row):
        return row.executionState == RemoteExecutionState.running

    started = wait_invocation(client, run_id, "slow", running)
    if restart := remote_daemon[2]:
        restart()
    daemon.restart(crash=True)
    client = daemon.connect()
    wait_invocation(client, run_id, "slow", lambda row: row.endedAt is not None)
    recovered = check_success(client, run_id, "slow", system)
    assert text(recovered.executionJobId) == text(started.executionJobId)
    assert client.call("RunStatusRequest", runId=run_id).state == RunState.failed
    after = client.call("RunRecordRequest", runId=run_id, card=path("after"))
    assert not after.exists

    run_id = start_run(client, "later")
    wait_invocation(client, run_id, "slow", running)
    client.call("RunCancelRequest", runId=run_id)
    assert wait_run(client, run_id, timeout=60).state == RunState.cancelled
    cancelled = client.call("RunRecordRequest", runId=run_id, card=path("slow")).record.summary
    assert cancelled.executionState == RemoteExecutionState.cancelled
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
