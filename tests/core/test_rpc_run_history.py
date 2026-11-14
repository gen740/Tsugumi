"""History portability and deletion, checked through public RPC."""

import json
import shutil

import pytest

from harness.fixtures import worker_project
from harness.observations import read_output, read_pages
from harness.scenario import FILE_OUTPUT, run, wait_run
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState


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
