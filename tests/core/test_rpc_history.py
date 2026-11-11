"""Recorded values, artifacts and logs observed through public RPC."""

import pytest

from harness.artifact_faults import corrupt_retained_file
from harness.fixtures import worker_project
from harness.observations import decode_scalar, read_output, read_pages
from harness.scenario import FILE_OUTPUT, run, wait_run
from projects import builtin_project, run_and_wait
from tsugumi_client import RpcFailure, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.RunState import RunState


CONTENTS = "".join(f"line {i:05d} 日本語テキスト\n" for i in range(20000))


@pytest.mark.parametrize("record_io,retain", [
    (False, False), (False, True), (True, False), (True, True),
], ids=["neither-record-nor-artifact", "artifact-only", "record-only", "record-and-artifact"])
def test_record_and_artifact_are_independent(core_factory, project, tmp_path, record_io, retain):
    """Recording values and retaining artifacts are independent choices.

    Given each combination of recording and artifact retention,
    when a file-producing card runs, the cache is cleared, and the daemon restarts,
    then only the requested records and artifacts remain readable.
    """
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path, [
        {"op": "file", "port": "doc", "path": "big.txt", "text": CONTENTS},
    ], outputs=FILE_OUTPUT, recordIO=record_io, retainArtifact=retain, artifactOutput="doc")
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    artifact_id = None

    def check(connection):
        nonlocal artifact_id
        record = connection.call("RunRecordRequest", runId=run_id, card=path("work"))
        assert bool(record.recordingDisabled) is not record_io
        assert len(record.record.outputs) == int(record_io)
        artifacts = connection.call("ArtifactListRequest", runId=run_id, card=path("work")).artifacts
        assert len(artifacts) == int(retain)
        if retain:
            current_id = text(artifacts[0].artifactId)
            artifact_id = current_id if artifact_id is None else artifact_id
            assert current_id == artifact_id
            if record_io:
                assert text(record.record.outputs[0].artifactId) == artifact_id
            contents = read_pages(connection, "ArtifactReadRequest", page_bytes=65536,
                                  artifactId=artifact_id)
            assert contents.decode() == CONTENTS
        if record_io:
            assert decode_scalar(read_output(connection, run_id, port="doc")).endswith("/big.txt")

    check(client)
    client.call("ProjectCacheClearRequest", opId=new_id())
    check(client)
    daemon.restart()
    check(daemon.connect())


def test_artifact_members_cannot_escape_the_artifact(core_factory, project, tmp_path):
    """An artifact read cannot reach a path outside the selected artifact."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    worker_project(client, project, tmp_path, [
        {"op": "file", "port": "doc", "path": "report.txt", "text": "retained"},
    ], outputs=FILE_OUTPUT, retainArtifact=True, artifactOutput="doc")
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    artifact = client.call("ArtifactListRequest", runId=run_id).artifacts[0]

    with pytest.raises(RpcFailure) as failure:
        client.call("ArtifactReadRequest", artifactId=text(artifact.artifactId), member="../../outside.txt")
    assert failure.value.kind in {"invalid_path", "invalid_request", "not_found"}
    assert read_pages(client, "ArtifactReadRequest", artifactId=text(artifact.artifactId)) == b"retained"


@pytest.mark.parametrize("already_read", [False, True], ids=["before-first-read", "after-successful-read"])
def test_a_corrupted_retained_artifact_is_rejected(core_factory, project, tmp_path, already_read):
    """Artifact reads report integrity failure when saved content has been changed.

    Given a retained file and a cleared execution cache,
    when its saved bytes are corrupted before or after the first successful read,
    then the next artifact read reports integrity_error instead of returning bytes.
    """
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    original = f"unique retained output {new_id()}\n".encode()
    worker_project(client, project, tmp_path, [
        {"op": "file", "port": "doc", "path": "report.txt", "text": original.decode()},
    ], outputs=FILE_OUTPUT, retainArtifact=True, artifactOutput="doc")
    run_id = run(client)
    wait_run(client, run_id)
    artifact = client.call("ArtifactListRequest", runId=run_id).artifacts[0]
    artifact_id = text(artifact.artifactId)
    client.call("ProjectCacheClearRequest", opId=new_id())
    if already_read:
        assert read_pages(client, "ArtifactReadRequest", artifactId=artifact_id) == original

    corrupt_retained_file(project, original)

    with pytest.raises(RpcFailure) as failure:
        client.call("ArtifactReadRequest", artifactId=artifact_id)
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
        assert page.offset == offset
        assert page.nextOffset == offset + len(content.encode())
        pages.append(content)
        if page.nextOffset >= page.sizeBytes:
            break
        assert page.nextOffset > offset
        offset = page.nextOffset
    assert "".join(pages) == expected


def test_past_invocations_keep_their_own_state(core_factory, project):
    """An older run keeps its own identity and timestamps after a newer run."""
    builtin_project(project)
    client = core_factory().connect()
    first, first_status = run_and_wait(client, "src", timeout=10)
    second, second_status = run_and_wait(client, "src", timeout=10)
    assert first_status.state == second_status.state == RunState.succeeded
    assert first != second

    old_record = client.call("RunRecordRequest", runId=first, card=path("src"))
    new_record = client.call("RunRecordRequest", runId=second, card=path("src"))
    assert text(old_record.run.status.runId) == first
    assert text(new_record.run.status.runId) == second
    assert old_record.record.summary.endedAt <= new_record.record.summary.startedAt


def test_a_client_opening_later_gets_each_cards_latest_invocation(core_factory, project):
    """A newly connected client restores one latest invocation for every card."""
    builtin_project(project)
    daemon = core_factory()
    client = daemon.connect()
    run_id, status = run_and_wait(client, "src", timeout=10)
    assert status.state == RunState.succeeded

    later = daemon.connect()
    page = later.call("RunLatestInvocationsRequest", runId=run_id)
    records = {"/".join(text(segment) for segment in record.card.segments): record for record in page.records}
    assert len(records) == len(page.records)
    assert {"src", "out"} <= records.keys()
    assert all(records[name].state == CardState.succeeded and records[name].invocation == 1
               for name in ("src", "out"))
    with pytest.raises(RpcFailure) as failure:
        later.call("RunLatestInvocationsRequest", runId=new_id())
    assert failure.value.kind == "not_found"
