"""Regressions exercised through public execution and history requests."""

from harness.fixtures import add_worker, worker_project
from harness.observations import decode_scalar, read_output
from harness.scenario import FILE_OUTPUT, Project, run, wait_run
from tsugumi_client import path
from Tsugumi.Data.RunState import RunState


def test_changed_card_package_replaces_a_resident_worker(project, core_factory, tmp_path):
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project)
    for name in ("original", "updated"):
        add_worker(graph, "work", tmp_path / name, [
            {"op": "output", "port": "out", "type": "string", "value": name},
        ], reuseWorker=True).save(client)
        run_id = run(client)
        assert wait_run(client, run_id).state == RunState.succeeded
        assert decode_scalar(read_output(client, run_id)) == name
        record, = client.call("RunInvocationsRequest", runId=run_id, card=path("work")).records
        assert not record.workerReused


def test_invalid_artifact_is_a_run_failure_not_a_service_crash(project, core_factory, tmp_path):
    """An unreadable artifact fails its run while the project stays available."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path, [
        {"op": "file", "port": "doc", "path": "data", "text": "kept"},
        {"op": "artifact_symlink", "name": "link", "target": "data"},
    ], outputs=FILE_OUTPUT)

    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.failed
    assert client.call("ProjectInspectRequest").rev >= 1
