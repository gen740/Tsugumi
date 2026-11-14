"""History portability and deletion, checked through public RPC."""

import json
import shutil
import time

import pytest

from harness.fixtures import worker_project
from harness.observations import read_output, read_pages
from harness.scenario import FILE_OUTPUT, Gate, run, wait_run
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.RunEventValue import RunEventValue


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
    original = core_factory(mocked_builds=True, mocked_worker=True, history_dir=tmp_path / "history-original")
    client = original.connect()
    prepare_history(client, project, tmp_path)
    run_ids = [successful_run(client)[0] for _ in range(2)]
    original_value = read_output(client, run_ids[0], port="doc")
    original.stop()

    copied_project = tmp_path / "copy"
    shutil.copytree(project, copied_project, symlinks=True)
    copied = core_factory(project_root=copied_project, mocked_builds=True, mocked_worker=True,
                          history_dir=tmp_path / "history-copy")
    client = copied.connect()
    assert listed(client) == list(reversed(run_ids))
    assert read_output(client, run_ids[0], port="doc") == original_value
    assert text(client.call("LogRangeRequest", runId=run_ids[0], card=path("work"),
                            invocation=1, stream=1).text) == "a recorded log\n"
    artifact = client.call("ArtifactListRequest", runId=run_ids[0], card=path("work")).artifacts[0]
    assert read_pages(client, "ArtifactReadRequest", page_bytes=4096,
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
    assert text(usage.protectedRun) == run_ids[-1]


def test_save_run_history_can_be_disabled_and_enabled(core_factory, project, tmp_path):
    """Enabling project history makes earlier local runs portable too."""
    original = core_factory(mocked_builds=True, mocked_worker=True, history_dir=tmp_path / "history-original")
    client = original.connect()
    prepare_history(client, project, tmp_path)

    def save_history(enabled):
        snapshot = client.call("ProjectInspectRequest")
        document = json.loads(text(snapshot.projectJson))
        document["saveRunHistory"] = enabled
        client.call("ProjectSaveRequest", baseRev=snapshot.rev, opId=new_id(), origin="ui:test",
                    changes=[change("SetProject", json=json.dumps(document))])

    save_history(False)
    first, _ = successful_run(client)
    original.stop()
    without_history = tmp_path / "without-history"
    shutil.copytree(project, without_history, symlinks=True)
    copy = core_factory(project_root=without_history, history_dir=tmp_path / "history-empty")
    assert listed(copy.connect()) == []
    copy.stop()

    original.start()
    client = original.connect()
    assert listed(client) == [first]
    save_history(True)
    original.stop()
    with_history = tmp_path / "with-history"
    shutil.copytree(project, with_history, symlinks=True)
    restored = core_factory(project_root=with_history, history_dir=tmp_path / "history-restored")
    assert listed(restored.connect()) == [first]


def test_deleting_history_removes_records_and_artifacts_without_resurrection(core_factory, project, tmp_path):
    """Deleting runs removes their public records and artifacts, including after restart."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True, history_dir=tmp_path / "history")
    client = daemon.connect()
    prepare_history(client, project, tmp_path)
    runs = [successful_run(client) for _ in range(3)]
    run_ids = [run_id for run_id, _ in runs]
    artifact_ids = [text(client.call("ArtifactListRequest", runId=run_id).artifacts[0].artifactId)
                    for run_id in run_ids]

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
    restored = core_factory(project_root=copied_project, history_dir=tmp_path / "history-copy")
    assert listed(restored.connect()) == []


def test_history_deletion_keeps_a_run_that_finished_after_selection(core_factory, project, tmp_path):
    """Deletion removes exactly the selected runs, even if another run finishes meanwhile."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True, history_dir=tmp_path / "history")
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
    restored = core_factory(project_root=copied_project, history_dir=tmp_path / "history-copy")
    assert listed(restored.connect()) == [second]


def test_immediate_reruns_after_finished_keep_accepting(core_factory, project, tmp_path):
    """A new run can be accepted immediately after each finished notification."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    worker_project(client, project, tmp_path, [
        {"op": "output", "port": "out", "type": "string", "value": "finished"},
    ], recordIO=True)
    rev = client.open().snapshot.rev

    for _ in range(100):
        # RunFinished is sent just before the scheduler releases the active
        # run. A brief busy reply is valid; a blocked acceptance is not.
        deadline = time.monotonic() + 5
        while True:
            try:
                accepted = client.call("RunRequest", baseRev=rev, opId=new_id(), origin="ui:test",
                                       card=path("work"), timeout=5)
                break
            except RpcFailure as failure:
                assert failure.kind == "busy"
                assert time.monotonic() < deadline, "scheduler did not release the finished run"
        run_id = text(accepted.run.runId)
        note = client.wait_notification(
            lambda n: n.valueType == NotificationPayload.RunEvent
            and text(n.value.runId) == run_id and n.value.valueType == RunEventValue.RunFinished,
            timeout=5,
        )
        assert note.value.value.summary.status.state == RunState.succeeded
