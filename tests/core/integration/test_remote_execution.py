"""Opt-in SSH execution against a disposable, already configured runner.

Set TSUGUMI_REMOTE_TEST_SSH_HOST to an SSH config alias and
TSUGUMI_REMOTE_TEST_GATEWAY to the absolute gateway executable on that host.
Run these cases with tsugumi-test-env and pytest -m environment.
"""

import json
import os
import select
import subprocess
import time
from pathlib import Path

import pytest
from harness.observations import read_output
from harness.values import reference_json
from projects import FILE_FBS, card, conn, run_and_wait, start_run, wait_run, write
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RemoteExecutionState import RemoteExecutionState
from Tsugumi.Data.RunState import RunState
from tsugumi_client import RpcFailure, path, text

pytestmark = pytest.mark.environment

RUN_LIST_OUTPUT = 'table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;'
# Each SSH job realizes its dependencies in a private store, even during a batch.
RUN_LIST_TIMEOUT_SECONDS = 1200
RUN_LIST_REMOTE = {
    "language": "bash", "recordIO": True, "executionService": "ssh-test",
    "executionResources": {"cpuMillis": 1000, "memoryBytes": 2 << 30},
    "timeoutSeconds": RUN_LIST_TIMEOUT_SECONDS, "_outputs": RUN_LIST_OUTPUT,
}

BROKER = """
import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
const [modulePath, support, project, host, gateway] = process.argv.slice(2);
const { authorizeExecutionProject, probeExecutionService, saveExecutionServices, startExecutionBroker } =
  await import(pathToFileURL(modulePath));
const control = path.join(support, "control");
fs.mkdirSync(control, {recursive: true, mode: 0o700});
const st = fs.statSync(project, {bigint: true});
const token = authorizeExecutionProject(support, [st.dev, st.ino, st.birthtimeNs || st.ctimeNs].join(":"), control);
const entry = await probeExecutionService({id: "ssh-test", name: "SSH test", sshHost: host, gateway});
saveExecutionServices(support, [entry]);
const endpoint = await startExecutionBroker(support);
console.log(JSON.stringify({TSUGUMI_EXECUTION_BROKER: endpoint, TSUGUMI_EXECUTION_BROKER_TOKEN_FILE: token}));
setInterval(() => {}, 30000);
"""


@pytest.fixture
def remote_broker(project, tmp_path, monkeypatch):
    host = os.environ.get("TSUGUMI_REMOTE_TEST_SSH_HOST")
    gateway = os.environ.get("TSUGUMI_REMOTE_TEST_GATEWAY")
    if not host or not gateway:
        pytest.skip("set TSUGUMI_REMOTE_TEST_SSH_HOST and TSUGUMI_REMOTE_TEST_GATEWAY for a dedicated SSH runner")
    script = tmp_path / "broker.mjs"
    script.write_text(BROKER)
    module = Path(__file__).resolve().parents[3] / "web/server/executionServices.ts"
    process = subprocess.Popen(["node", str(script), str(module), str(tmp_path / "support"),
                                str(project), host, gateway], stdout=subprocess.PIPE, text=True)
    try:
        ready, _, _ = select.select([process.stdout], [], [], 30)
        assert ready, "SSH broker did not start"
        line = process.stdout.readline()
        assert line, "SSH broker exited before starting"
        for key, value in json.loads(line).items():
            monkeypatch.setenv(key, value)
        yield
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def fixture(project):
    script = """
      set -e
      test "$(uname -s)" = Linux
      test ! -e /etc/hostname
      test -z "$SSH_AUTH_SOCK"
      printf 'remote-card-stdout\\n'
      cp "$FILE_IN" "$TSUGUMI_OUTPUT_DIR/copied.txt"
      jq -nc --arg f "$TSUGUMI_OUTPUT_DIR/copied.txt" '{doc:{value:$f}}' > "$TSUGUMI_OUTPUT"
    """
    source = {"template": "input", "params": [{"name": "value", "type": "string", "data": "payload.txt"}],
              "_outputs": FILE_FBS}
    settings = {
        "language": "bash", "inputs": [{"name": "in"}], "_outputs": FILE_FBS,
        "executionService": "ssh-test", "executionResources": {"cpuMillis": 2500, "memoryBytes": 4 << 30},
        "retainArtifact": True, "artifactOutput": "doc", "recordIO": True, "timeoutSeconds": 600,
    }
    definitions = {
        "input": source,
        "work": {**settings, "_nix": "{ pkgs, inputs, params }: { run = ''" + script + "''; }"},
        "slow": {**settings, "_nix": "{ pkgs, inputs, params }: { run = ''sleep 25\n" + script + "''; }"},
    }
    write(project, definitions,
          [card("source", "input"), card("later", "input"), card("work", "work"), card("slow", "slow")],
          [conn("source", "doc", "work", "in"), conn("later", "doc", "slow", "in")],
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
    assert text(summary.executionService) == "ssh-test"
    assert text(summary.executionSystem) == system
    assert len(text(summary.executionJobId)) == 64
    assert summary.executionResources.cpuMillis == 2500
    assert summary.executionResources.memoryBytes == 4 << 30
    artifacts = client.call("ArtifactListRequest", runId=run_id, card=path(name)).artifacts
    assert len(artifacts) == 1
    content = client.call("ArtifactReadRequest", artifactId=text(artifacts[0].artifactId), limitBytes=1024)
    assert bytes(content.bytes) == b"remote-input-roundtrip\n"
    logs = client.call("LogRangeRequest", runId=run_id, card=path(name), invocation=1, stream=LogStream.stdout)
    assert text(logs.text) == "remote-card-stdout\n"
    environments = client.call("RunEnvironmentsRequest", runId=run_id).environments
    assert environments and text(environments[0].system) == system
    return summary


def test_ssh_card_roundtrip_reconnect_and_cancel(project, core_factory, remote_broker):
    fixture(project)
    daemon = core_factory()
    client = daemon.connect()
    services = client.call("ProjectInspectRequest").executionServices
    remote = next(service for service in services if text(service.id) == "ssh-test")
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
    daemon.restart(crash=True)
    client = daemon.connect()
    wait_invocation(client, run_id, "slow", lambda row: row.endedAt is not None)
    recovered = check_success(client, run_id, "slow", system)
    assert text(recovered.executionJobId) == text(started.executionJobId)
    assert client.call("RunStatusRequest", runId=run_id).state == RunState.failed

    run_id = start_run(client, "later")
    wait_invocation(client, run_id, "slow", running)
    client.call("RunCancelRequest", runId=run_id)
    assert wait_run(client, run_id, timeout=60).state == RunState.cancelled
    cancelled = client.call("RunRecordRequest", runId=run_id, card=path("slow")).record.summary
    assert cancelled.executionState == RemoteExecutionState.cancelled
    assert not client.call("ArtifactListRequest", runId=run_id, card=path("slow")).artifacts


def test_ssh_run_list_preserves_implementation_order(project, core_factory, remote_broker, tmp_path):
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
        assert text(row.executionService) == "ssh-test"
        assert len(text(row.executionJobId)) == 64
    assert len({text(row.executionJobId) for row in page.records}) == 4

    record = client.call("RunRecordRequest", runId=run_id, card=path("collect")).record
    output, = record.outputs
    payload = read_output(client, run_id, card="collect")
    collected = reference_json(tmp_path / "decode", bytes(output.schemaBfbs), payload,
                               root_type=text(output.rootType))
    assert collected["items"] == [{"value": value} for value in (110, 210, 120, 220)]


def test_ssh_run_list_failure_cancels_running_sibling(project, core_factory, remote_broker):
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
