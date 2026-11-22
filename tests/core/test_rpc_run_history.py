"""History portability and deletion, checked through public RPC."""

import shutil
from contextlib import closing

import pytest

from harness.fixtures import add_worker, worker_project
from harness.host import Host, HostConnection, HostFailure
from harness.observations import read_output, read_pages
from harness.scenario import FILE_OUTPUT, Gate, Project, run, wait_run
from tsugumi_client import RpcFailure, new_id, path, text
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.RunEventValue import RunEventValue
from Tsugumi.RPC.RunRecordCategory import RunRecordCategory


CONTENTS = "retained history 日本語\n" * 500


def listed(client):
    return [text(run.status.runId) for run in client.call("RunHistoryRequest", limit=100).runs]


def prepare_history(client, project, directory):
    worker_project(client, project, directory, [
        {"op": "log", "stream": "stdout", "text": "a recorded log"},
        {"op": "file", "port": "doc", "path": "report.txt", "text": CONTENTS},
    ], outputs=FILE_OUTPUT, recordIO=True, retainArtifact=True, artifactOutput="doc")


def successful_run(client):
    run_id = run(client)
    status = wait_run(client, run_id)
    assert status.state == RunState.succeeded
    return run_id, status


def test_project_copy_restores_history_and_merges_new_runs(core_factory, project, tmp_path):
    """Copying a project preserves its runs, values, logs and retained artifacts.

    Given a project with completed runs stored on a separate history disk,
    when its project directory is copied and opened with an empty history disk,
    then the public history is restored and runs added by the copy can return with it.
    """
    original = core_factory(mocked_builds=True, mocked_worker=True)
    client = original.connect()
    prepare_history(client, project, tmp_path)
    run_ids = [successful_run(client)[0] for _ in range(2)]
    original_value = read_output(client, run_ids[0], port="doc")
    original.stop()

    copied_project = tmp_path / "copy"
    shutil.copytree(project, copied_project, symlinks=True)
    copied = core_factory(project_root=copied_project, mocked_builds=True, mocked_worker=True)
    client = copied.connect()
    assert listed(client) == list(reversed(run_ids))
    assert read_output(client, run_ids[0], port="doc") == original_value
    assert text(client.call("LogRangeRequest", runId=run_ids[0], card=path("work"),
                            invocation=1, stream=1).text) == "a recorded log\n"
    artifact = client.call("ArtifactListRequest", runId=run_ids[0], card=path("work")).artifacts[0]
    assert read_pages(client, "ArtifactReadRequest", page_bytes=4096, verify_integrity=True,
                      artifactId=text(artifact.artifactId)).decode() == CONTENTS
    new_run, _ = successful_run(client)
    copied.stop()

    # This models moving the user's complete project back, without depending
    # on the names, schema, or layout of core's private history files.
    shutil.rmtree(project)
    shutil.copytree(copied_project, project, symlinks=True)
    original.start()
    assert set(listed(original.connect())) == set(run_ids + [new_run])
    # Copy/import order never changes which local scratch is protected.
    usage = original.connect().call("ProjectCacheUsageRequest")
    assert text(usage.protectedRun) == new_run



def test_deleting_history_removes_records_and_artifacts_without_resurrection(core_factory, project, tmp_path):
    """Deleting runs removes their public records and artifacts, including after restart."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    prepare_history(client, project, tmp_path)
    runs = [successful_run(client) for _ in range(3)]
    run_ids = [run_id for run_id, _ in runs]
    artifacts = [client.call("ArtifactListRequest", runId=run_id).artifacts[0] for run_id in run_ids]
    artifact_ids = [text(artifact.artifactId) for artifact in artifacts]
    assert artifact_ids == [f"{run_id}/work/1/doc" for run_id in run_ids]
    assert all(text(artifact.name) == "report.txt" for artifact in artifacts)

    environment, = client.call("RunEnvironmentsRequest", runId=run_ids[0]).environments
    environment_id = text(environment.id)

    operation = new_id()
    cutoff = runs[1][1].endedAt + 1
    deleted = client.call("RunHistoryDeleteRequest", opId=operation, endedBefore=cutoff)
    assert deleted.deletedRuns == 2 and deleted.freedBytes > 0
    assert listed(client) == [run_ids[2]]
    for run_id, artifact_id in zip(run_ids[:2], artifact_ids[:2]):
        with pytest.raises(RpcFailure) as failure:
            client.call("RunRecordRequest", runId=run_id, card=path("work"))
        assert failure.value.kind == "not_found"
        with pytest.raises(RpcFailure) as failure:
            client.call("ArtifactReadRequest", artifactId=artifact_id)
        assert failure.value.kind == "not_found"
    assert read_pages(client, "ArtifactReadRequest", page_bytes=4096,
                      artifactId=artifact_ids[2]).decode() == CONTENTS

    replay = client.call("RunHistoryDeleteRequest", opId=operation, endedBefore=cutoff)
    assert replay.deletedRuns == 2
    assert client.call("RunHistoryDeleteRequest", opId=new_id()).deletedRuns == 1
    daemon.restart()
    assert listed(daemon.connect()) == []
    daemon.stop()
    copied_project = tmp_path / "copy"
    shutil.copytree(project, copied_project, symlinks=True)
    restored = core_factory(project_root=copied_project)
    client = restored.connect()
    assert listed(client) == []
    with pytest.raises(RpcFailure) as failure:
        client.call("RunEnvironmentPathsRequest", runId=run_ids[0], environmentId=environment_id)
    assert failure.value.kind == "not_found"


def test_history_deletion_keeps_a_run_that_finished_after_selection(core_factory, project, tmp_path):
    """Deletion removes exactly the selected runs, even if another run finishes meanwhile."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    prepare_history(client, project, tmp_path / "first")
    first, _ = successful_run(client)
    first_artifact = text(client.call("ArtifactListRequest", runId=first).artifacts[0].artifactId)
    gate = Gate(tmp_path, "second")
    worker_project(client, project, tmp_path / "second", [
        gate.action(),
        {"op": "file", "port": "doc", "path": "report.txt", "text": CONTENTS},
    ], outputs=FILE_OUTPUT, recordIO=True, retainArtifact=True, artifactOutput="doc")
    second = run(client)
    gate.wait()
    selected = [text(row.status.runId) for row in client.call("RunHistoryRequest", limit=100).runs
                if row.status.state in {RunState.succeeded, RunState.failed, RunState.cancelled}]
    assert selected == [first]
    gate.release()
    assert wait_run(client, second).state == RunState.succeeded
    second_artifact = text(client.call("ArtifactListRequest", runId=second).artifacts[0].artifactId)

    assert client.call("RunHistoryDeleteRequest", opId=new_id(),
                       restrictToRunIds=True, runIds=[]).deletedRuns == 0
    with pytest.raises(RpcFailure) as failure:
        client.call("RunHistoryDeleteRequest", opId=new_id(), runIds=selected)
    assert failure.value.kind == "invalid_request"
    operation = new_id()
    assert client.call("RunHistoryDeleteRequest", opId=operation,
                       restrictToRunIds=True, runIds=selected).deletedRuns == 1
    assert client.call("RunHistoryDeleteRequest", opId=operation,
                       restrictToRunIds=True, runIds=selected).deletedRuns == 1
    with pytest.raises(RpcFailure) as failure:
        client.call("RunHistoryDeleteRequest", opId=operation,
                    restrictToRunIds=True, runIds=[second])
    assert failure.value.kind == "operation_conflict"
    with pytest.raises(RpcFailure) as failure:
        client.call("ArtifactReadRequest", artifactId=first_artifact)
    assert failure.value.kind == "not_found"

    daemon.restart()
    client = daemon.connect()
    assert listed(client) == [second]
    assert read_output(client, second, port="doc")
    assert read_pages(client, "ArtifactReadRequest", artifactId=second_artifact).decode() == CONTENTS
    daemon.stop()
    copied_project = tmp_path / "copy"
    shutil.copytree(project, copied_project, symlinks=True)
    restored = core_factory(project_root=copied_project)
    assert listed(restored.connect()) == [second]


def test_immediate_reruns_after_finished_keep_accepting(core_factory, project, tmp_path):
    """A new run can be accepted immediately after each finished notification."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    worker_project(client, project, tmp_path, [
        {"op": "output", "port": "out", "type": "string", "value": "finished"},
    ], recordIO=True)
    rev = client.open().snapshot.rev

    for _ in range(2):
        # RunFinished is sent just before the scheduler releases the active
        # run. The active-slot release has no separate notification, so a
        # brief busy reply is valid; a blocked acceptance is not.
        while True:
            try:
                accepted = client.call("RunRequest", baseRev=rev, opId=new_id(), origin="ui:test",
                                       card=path("work"))
                break
            except RpcFailure as failure:
                assert failure.kind == "busy"
        run_id = text(accepted.run.runId)
        note = client.wait_notification(
            lambda n: n.valueType == NotificationPayload.RunEvent
            and text(n.value.runId) == run_id and n.value.valueType == RunEventValue.RunFinished,
        )
        assert note.value.value.summary.status.state == RunState.succeeded


def test_category_deletion_through_host_keeps_summaries_and_marks_forwarded_files(core_factory, project, tmp_path):
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    graph = Project(project)
    outputs = FILE_OUTPUT.replace("table Outputs", "table Label { value:string; } table Outputs").replace(
        'doc:File (tsugumi:"file");', 'doc:File (tsugumi:"file"); label:Label;')
    add_worker(graph, "work", tmp_path / "package", [
        {"op": "log", "stream": "stdout", "text": "recorded"},
        {"op": "file", "port": "doc", "path": "report.txt", "text": CONTENTS},
        {"op": "output", "port": "label", "type": "string", "value": "keep its summary"},
    ], outputs=outputs, retainArtifact=True, artifactOutput="doc")
    graph.card("fork", {"type": "fork", "retainArtifact": True})
    graph.connect("work", "doc", "fork", "value").save(client)
    source = run(client, propagate=True)
    assert wait_run(client, source).state == RunState.succeeded
    forwarded = run(client, card="fork", propagate=False)
    assert wait_run(client, forwarded).state == RunState.succeeded
    project_id = daemon.ready["project_id"]

    def record(run_id, card="work"):
        return client.call("RunRecordRequest", runId=run_id, card=path(card)).record

    before = record(source)
    owner = next(v for v in before.outputs if text(v.name) == "doc")
    assert all(v.filePath == owner.filePath for v in record(forwarded, "fork").outputs)

    with closing(Host(tmp_path / "host")) as host:
        with pytest.raises(HostFailure) as denied:
            HostConnection(dict(host.ready, user_token=host.ready["external_token"]))
        assert denied.value.code == "permission_denied"
        host.call("RegisterProjectRequest", projectId=project_id, endpoint=f"unix:{daemon.unix}")
        args = dict(projectId=project_id, runId=source)
        refs = host.call("RunFileReferencesRequest", **args)
        assert list(map(text, refs.runIds)) == [forwarded]

        def delete(category, op=None):
            return host.call("RunCategoryDeleteRequest", **args, category=category, opId=op or new_id())

        delete(RunRecordCategory.Values)
        assert [text(v.name) for v in record(source).outputs] == ["doc"]
        delete(RunRecordCategory.Logs)
        assert not client.call("LogRangeRequest", runId=source, stream=1).exists
        assert client.call("LogRangeRequest", runId=source, stream=3).exists

        operation = new_id()
        assert text(delete(RunRecordCategory.Files, operation).runId) == source
        assert text(delete(RunRecordCategory.Files, operation).runId) == source
        assert not client.call("ArtifactListRequest", runId=source).artifacts
        assert not record(source).outputs
        assert record(source).summary.state == before.summary.state
        assert record(source).summary.durationMs == before.summary.durationMs
        assert all(v.fileDeleted and not v.filePath for v in record(forwarded, "fork").outputs)

    daemon.restart()
    client = daemon.connect()
    assert listed(client) == [forwarded, source]
    assert all(v.fileDeleted for v in record(forwarded, "fork").outputs)
