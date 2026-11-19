"""File inputs of a child Workflow remain usable for repeated partial runs."""

import pytest

from projects import FILE_FBS, card, card_in_card, conn, run_and_wait, write
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState
from Tsugumi.Data.FailureKind import FailureKind


CONTENTS = "the saved input must remain readable\n"


@pytest.fixture
def saved_file_subgraph(project, core_factory):
    write(
        project,
        defs={
            "source": {
                "template": "input",
                "params": [{"name": "value", "type": "string", "data": "report.txt"}],
                "_outputs": FILE_FBS,
            },
            "copy": {
                "template": "fork",
                "trigger": "eachInput",
                "recordIO": True,
                "retainArtifact": True,
                "artifactOutput": "out1",
            },
        },
        cards=[
            card("source", "source"),
            card_in_card(
                "child", [card("inner", "copy")], [],
                {"inputs": {"in": {"card": "inner", "port": "in"}},
                 "outputs": {"out": {"card": "inner", "port": "out1"}}},
            ),
        ],
        conns=[conn("source", "doc", "child", "in")],
        files={"definitions/source/report.txt": CONTENTS},
    )
    client = core_factory().connect()
    _, missing = run_and_wait(client, "child", propagate=False)
    assert missing.state == RunState.failed
    assert missing.failure.kind == FailureKind.input_missing
    run_id, status = run_and_wait(client, "source")
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    return client, run_id


def artifact_contents(client, run_id):
    artifacts = client.call("ArtifactListRequest", runId=run_id, card=path("child", "inner")).artifacts
    assert len(artifacts) == 1
    content = client.call("ArtifactReadRequest", artifactId=text(artifacts[0].artifactId), limitBytes=4096)
    assert content.eof is True
    return bytes(content.bytes).decode()


def test_each_input_child_preserves_saved_files_across_partial_runs(saved_file_subgraph):
    """Repeated partial runs can reuse a nested Workflow's saved file input."""
    client, first_run = saved_file_subgraph
    assert artifact_contents(client, first_run) == CONTENTS
    for _ in range(2):
        run_id, status = run_and_wait(client, "child", propagate=False)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        assert status.partialRun
        assert artifact_contents(client, run_id) == CONTENTS
        record = client.call("RunRecordRequest", runId=run_id, card=path("child", "inner"))
        assert record.run.status.partialRun
        assert record.record.outputs  # Explicit Record I/O and Retain artifact are both obeyed.


def test_name_changes_preserve_saved_file_inputs_and_evidence(saved_file_subgraph):
    """Container, child and producer names leave saved paths and source JSON intact."""
    from test_definitions import rename
    client, first_run = saved_file_subgraph
    record = client.call("RunRecordRequest", runId=first_run, card=path("child", "inner"))
    import flatbuffers
    def packed(value):
        builder = flatbuffers.Builder(1024)
        builder.Finish(value.Pack(builder))
        return bytes(builder.Output())
    before = packed(record)
    rename(client, "source", "Same name")
    rename(client, "child", "Same name")
    rename(client, "inner", "Same name", "externals/child/")
    assert packed(client.call("RunRecordRequest", runId=first_run, card=path("child", "inner"))) == before
    assert artifact_contents(client, first_run) == CONTENTS
    run_id, status = run_and_wait(client, "child", propagate=False)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert status.partialRun
    assert artifact_contents(client, run_id) == CONTENTS
    record = client.call("RunRecordRequest", runId=run_id, card=path("child", "inner"))
    assert record.run.status.partialRun
    assert record.record.outputs  # Explicit Record I/O and Retain artifact are both obeyed.
