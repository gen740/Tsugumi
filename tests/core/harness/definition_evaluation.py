"""Observations shared by definition correctness and evaluation economy tests."""

import json

from harness.scenario import run, wait_run
from tsugumi_client import new_id, path, text
from Tsugumi.Data.RunState import RunState


EMPTY_OUTPUTS = "table Outputs {} root_type Outputs;"


def reload_project(client):
    return client.call("ProjectReloadRequest", baseRev=client.call("ProjectInspectRequest").rev,
                       opId=new_id(), origin="user")


def edit_json(file, edit):
    value = json.loads(file.read_text())
    edit(value)
    file.write_text(json.dumps(value))


def completed_definition(client, card="work", *, start=None):
    """Return a fresh run's output, actual definition identity and invocation."""
    run_id = run(client, start or card)
    status = wait_run(client, run_id, timeout=1800)
    logs = [text(client.call("LogRangeRequest", runId=run_id, card=path(card),
                             stream=stream, limitBytes=262144).text) for stream in (1, 2)]
    assert status.state == RunState.succeeded, (status.failure and text(status.failure.diagnostic), logs)
    invocation, = client.call("RunInvocationsRequest", runId=run_id, card=path(card)).records
    number = text(invocation.environmentId)
    assert number
    environments = client.call("RunEnvironmentsRequest", runId=run_id).environments
    environment = next(e for e in environments if text(e.id) == number)
    assert text(environment.derivation).endswith(".drv")
    assert text(environment.manifest)
    record = client.call("RunRecordRequest", runId=run_id, card=path(card)).record
    assert text(record.summary.environmentId) == number
    # An environment's ID is a number within its run. What a run was built from
    # is the recorded derivation, manifest and system.
    identity = (text(environment.derivation), text(environment.manifest), text(environment.system))
    return logs[0], identity, invocation


def full_evaluations(daemon):
    """Count complete evaluator entries, independently of stages and build hits."""
    counter = daemon.gates / "full-evaluations"
    return int(counter.read_text()) if counter.exists() else 0
