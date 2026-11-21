"""Regressions exercised through public execution and history requests."""

import pytest

from harness.fixtures import worker_project
from harness.observations import decode_scalar, read_output
from harness.scenario import FILE_OUTPUT, STRING_OUTPUT, Project, run, wait_run
from Tsugumi.Data.RunState import RunState


def test_changed_shell_steps_are_used_on_the_next_run(project, core_factory):
    """Changing a Card's shell steps changes what its next run executes."""
    client = core_factory(mocked_builds=True).connect()
    body = 'printf \'{"out":{"value":"%s"}}\' "${MARK:-missing}" > "$TSUGUMI_OUTPUT"'
    definition = {"type": "code", "sandbox": False, "reuseWorker": True, "recordIO": True}
    graph = Project(project).card("work", definition, outputs=STRING_OUTPUT,
                                 manifest={"run": {"bash": body}, "preRun": "export MARK=old | cat", "postRun": "true"})
    graph.save(client)
    first = run(client)
    # A broken baseline must fail, even while the changed-script bug is expected.
    if wait_run(client, first).state != RunState.succeeded:
        pytest.fail("the original shell steps did not complete")
    try:
        first_value = decode_scalar(read_output(client, first))
    except AssertionError as error:
        pytest.fail(f"the original output could not be read: {error}")
    if first_value != "missing":
        pytest.fail(f"the original shell steps returned {first_value!r}, expected 'missing'")

    graph.card("work", definition, outputs=STRING_OUTPUT,
               manifest={"run": {"bash": body}, "preRun": "export MARK=old ", "postRun": " cat|true"}).save(client)
    second = run(client)
    assert wait_run(client, second).state == RunState.succeeded
    assert decode_scalar(read_output(client, second)) == "old"


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
