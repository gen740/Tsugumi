"""Desired public behavior for known defects; --runxfail exposes each failure.

The marks are strict so fixing production behavior requires removing the mark.
No test asserts a private core function, storage layout or implementation trace.
"""

import json
import shutil

import pytest

from harness.fixtures import worker_project
from harness.observations import decode_scalar, read_output
from harness.scenario import FILE_OUTPUT, STRING_OUTPUT, Project, run, wait_run
from projects import MSG_FBS, card, card_in_card, put, write
from tsugumi_client import new_id, text
from Tsugumi.Data.RunState import RunState


@pytest.mark.xfail(strict=True, raises=AssertionError, reason="Reload omits changes to link_json")
def test_reload_changes_an_external_card_target(project, core_factory):
    """Reload uses the external Card target the user just selected."""
    defs = {"input": {"template": "input", "_outputs": MSG_FBS,
                      "params": [{"name": "text", "type": "string", "data": "hello"}]}}
    write(project, defs, [card_in_card("nested", [card("source", "input")], [], {"inputs": {}, "outputs": {}})], [])
    shutil.copytree(project / "externals/nested", project / "externals/other")
    client = core_factory().connect()
    before = client.call("ProjectInspectRequest")
    # Reload deliberately imports a user's edit to a public project document.
    put(project / "definitions/nested/_link.json", '{"external":"other"}')
    client.call("ProjectReloadRequest", baseRev=before.rev, opId=new_id(), origin="ui:test")
    after = client.call("ProjectInspectRequest")
    definition = next(x for x in after.definitions if text(x.id) == "nested")
    assert json.loads(text(definition.linkJson))["external"] == "other"


def test_changed_shell_steps_are_used_on_the_next_run(project, core_factory):
    """Changing a Card's shell steps changes what its next run executes."""
    client = core_factory(mocked_builds=True).connect()
    body = 'printf \'{"out":{"value":"%s"}}\' "${MARK:-missing}" > "$TSUGUMI_OUTPUT"'
    definition = {"language": "bash", "sandbox": False, "reuseWorker": True, "recordIO": True}
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
