"""Regressions exercised through public execution and history requests."""

from harness.fixtures import add_worker, mock_plan, worker_project
from harness.native_card import native_card
from harness.observations import decode_scalar, read_output
from harness.scenario import FILE_OUTPUT, Project, run, wait_run
from tsugumi_client import path
from Tsugumi.Data.PortKind import PortKind
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
        {"op": "directory", "port": "doc", "path": "real", "files": {"file.txt": "kept"}},
        {"op": "symlink", "path": "alias", "target": "real"},
        {"op": "file", "port": "doc", "path": "alias/file.txt", "text": "kept"},
    ], outputs=FILE_OUTPUT, retainArtifact=True, artifactOutput="doc")

    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.failed
    assert client.call("ProjectInspectRequest").rev >= 1


def test_output_copy_failure_keeps_the_resident_worker(project, core_factory, tmp_path):
    package = native_card(tmp_path / "package", """
#include <stdio.h>

static int called;
int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    (void)call;
    puts("worker log");
    if (called++) {
        FILE *file = fopen("result", "w");
        if (!file) return 1;
        fputs("finished", file);
        fclose(file);
    }
    *result = (tsugumi_bytes){result_bytes, sizeof(result_bytes)};
    return 0;
}
""", [("doc", PortKind.Path, "result")])
    client = core_factory(mocked_builds=True).connect()
    Project(project).card("work", {"type": "code", "reuseWorker": True, "sandbox": {
        "filesystem": False, "pid": False, "ipc": False, "identity": False,
    }}, outputs=FILE_OUTPUT, plan=mock_plan([package])).save(client)

    first = run(client)
    assert wait_run(client, first).state == RunState.failed
    log = client.call("LogRangeRequest", runId=first, card=path("work"), invocation=1, stream=1)
    assert log.text == b"worker log\n"
    second = run(client)
    assert wait_run(client, second).state == RunState.succeeded
    record, = client.call("RunInvocationsRequest", runId=second, card=path("work")).records
    assert record.workerReused
