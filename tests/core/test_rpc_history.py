"""Recorded values, artifacts and logs observed through public RPC."""

from uuid import UUID

import pytest

from harness.artifact_faults import corrupt_retained_file
from harness.fixtures import worker_project
from harness.observations import read_pages
from harness.scenario import FILE_OUTPUT, run, wait_run
from projects import builtin_project, run_and_wait
from tsugumi_client import RpcFailure, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.RunState import RunState


CONTENTS = "".join(f"line {i:05d} 日本語テキスト\n" for i in range(20000))


@pytest.mark.parametrize("record_io", [False, True], ids=["artifact-only", "recorded-value"])
def test_artifacts_survive_without_value_recording(core_factory, project, tmp_path, record_io):
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path, [
        {"op": "file", "port": "doc", "path": "big.txt", "text": CONTENTS},
    ], outputs=FILE_OUTPUT, recordIO="on" if record_io else "off")
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    client.call("ProjectCacheClearRequest", opId=new_id())
    daemon.restart()
    client = daemon.connect()
    record = client.call("RunRecordRequest", runId=run_id, card=path("work"))
    assert bool(record.recordingDisabled) is not record_io
    assert len(record.record.outputs) == int(record_io)
    artifact, = client.call("ArtifactListRequest", runId=run_id, card=path("work")).artifacts
    artifact_id = text(artifact.artifactId)
    assert UUID(artifact_id).version == 4
    sequence, counter = map(int, text(artifact.label).split("."))
    assert sequence > 0 and counter == 0
    assert artifact.sizeBytes == len(CONTENTS.encode())
    assert text(artifact.treeHash).startswith("tree:")
    directory = project / ".tsugumi" / "artifacts" / run_id / "work" / "@1" / text(artifact.label).split(".")[1]
    assert directory.stat().st_mode & 0o222 == 0
    assert (directory / "big.txt").stat().st_mode & 0o222 == 0
    if record_io:
        value_artifact, = record.record.outputs[0].artifacts
        assert text(value_artifact.id) == artifact_id
        assert value_artifact.sizeBytes == artifact.sizeBytes
    contents = read_pages(client, "ArtifactReadRequest", page_bytes=65536,
                          artifactId=artifact_id, member="big.txt")
    assert contents.decode() == CONTENTS
    assert client.call("RunHistoryDeleteRequest", opId=new_id(),
                       restrictToRunIds=True, runIds=[run_id]).deletedRuns == 1
    assert not directory.exists()


def test_artifact_members_cannot_escape_the_artifact(core_factory, project, tmp_path):
    """An artifact read cannot reach a path outside the selected artifact."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    worker_project(client, project, tmp_path, [
        {"op": "file", "port": "doc", "path": "report.txt", "text": "retained"},
    ], outputs=FILE_OUTPUT)
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    artifact = client.call("ArtifactListRequest", runId=run_id).artifacts[0]

    with pytest.raises(RpcFailure) as failure:
        client.call("ArtifactReadRequest", artifactId=text(artifact.artifactId), member="../../outside.txt")
    assert failure.value.kind in {"invalid_path", "invalid_request", "not_found"}
    assert read_pages(client, "ArtifactReadRequest", artifactId=text(artifact.artifactId), member="report.txt") == b"retained"


def test_a_corrupted_retained_artifact_is_rejected(core_factory, project, tmp_path):
    """Artifact reads report integrity failure when saved content has been changed.

    Given a retained file and a cleared execution cache,
    when its saved bytes are corrupted after the first successful read,
    then the next artifact read reports integrity_error instead of returning bytes.
    """
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    original = f"unique retained output {new_id()}\n".encode()
    worker_project(client, project, tmp_path, [
        {"op": "file", "port": "doc", "path": "report.txt", "text": original.decode()},
    ], outputs=FILE_OUTPUT)
    run_id = run(client)
    wait_run(client, run_id)
    artifact = client.call("ArtifactListRequest", runId=run_id).artifacts[0]
    artifact_id = text(artifact.artifactId)
    client.call("ProjectCacheClearRequest", opId=new_id())
    assert read_pages(client, "ArtifactReadRequest", artifactId=artifact_id, member="report.txt") == original

    corrupt_retained_file(project / ".tsugumi/artifacts" / run_id / "work/@1" / text(artifact.label).split(".")[1],
                          original)

    with pytest.raises(RpcFailure) as failure:
        client.call("ArtifactReadRequest", artifactId=artifact_id, member="report.txt")
    assert failure.value.kind == "integrity_error"


def test_log_paging_keeps_utf8_boundaries(core_factory, project, tmp_path):
    """Paged logs preserve every multibyte character exactly once."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    expected = "日本語🌱 café\n" * 200
    worker_project(client, project, tmp_path, [
        {"op": "log", "stream": "stdout", "text": expected[:-1]},
        {"op": "output", "port": "out", "type": "string", "value": "done"},
    ])
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded

    pages = []
    offset = 0
    while True:
        page = client.call("LogRangeRequest", runId=run_id, card=path("work"), invocation=1,
                           stream=1, offset=offset, limitBytes=97)
        content = text(page.text)
        assert page.nextOffset == offset + len(content.encode())
        pages.append(content)
        if page.nextOffset >= page.sizeBytes:
            break
        assert page.nextOffset > offset
        offset = page.nextOffset
    assert "".join(pages) == expected



def test_a_client_opening_later_gets_each_cards_latest_invocation(core_factory, project):
    """A newly connected client restores one latest invocation for every card."""
    builtin_project(project)
    daemon = core_factory()
    client = daemon.connect()
    run_id, status = run_and_wait(client, "src")
    assert status.state == RunState.succeeded

    later = daemon.connect()
    page = later.call("RunLatestInvocationsRequest", runId=run_id)
    records = {"/".join(text(segment) for segment in record.card.segments): record for record in page.records}
    assert len(records) == len(page.records)
    assert {"src", "out"} <= records.keys()
    assert all(records[name].state == CardState.succeeded and records[name].invocation == 1
               for name in ("src", "out"))
    with pytest.raises(RpcFailure) as failure:
        later.call("RunLatestInvocationsRequest", runId="999999")
    assert failure.value.kind == "not_found"
