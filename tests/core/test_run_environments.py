"""Inspect recorded build environments through public RPC, without re-evaluation."""

import json
import shutil

import pytest

from harness.fixtures import worker_project
from harness.scenario import Project, STRING_OUTPUT, run, wait_run
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState


def inventory(client, run_id, card="work"):
    page = client.call("RunEnvironmentsRequest", runId=run_id, limit=1)
    assert len(page.environments) == 1
    assert page.nextOffset is None
    environment = page.environments[0]
    record = client.call("RunRecordRequest", runId=run_id, card=path(card))
    assert text(record.record.summary.environmentId) == text(environment.id)
    paths = []
    offset = 0
    while True:
        page = client.call("RunEnvironmentPathsRequest", runId=run_id,
                           environmentId=text(environment.id), offset=offset, limit=1)
        paths.extend(text(value) for value in page.paths)
        assert page.total == environment.pathCount
        if page.nextOffset is None:
            break
        assert page.nextOffset > offset
        offset = page.nextOffset
    assert len(paths) == environment.pathCount
    assert paths == sorted(set(paths))
    assert text(environment.manifest) in paths
    assert text(environment.derivation).endswith(".drv")
    return text(environment.id), paths


def test_environment_survives_cache_reuse_restart_and_project_copy(core_factory, project, tmp_path):
    """Cached runs and portable history retain the exact recorded environment."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True, history_dir=tmp_path / "history")
    client = daemon.connect()
    worker_project(client, project, tmp_path, [])
    runs = []
    for _ in range(2):
        run_id = run(client)
        assert wait_run(client, run_id).state == RunState.succeeded
        runs.append(run_id)
    expected = inventory(client, runs[0])
    assert inventory(client, runs[1]) == expected
    daemon.stop()
    copied_project = tmp_path / "copy"
    shutil.copytree(project, copied_project, symlinks=True)
    restored = core_factory(project_root=copied_project, mocked_builds=True,
                            history_dir=tmp_path / "restored-history")
    assert inventory(restored.connect(), runs[0]) == expected
    daemon.start()
    assert inventory(daemon.connect(), runs[1]) == expected


def test_failed_execution_keeps_its_environment(core_factory, project, tmp_path):
    """A worker failure still records the environment that was used."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path, [{"op": "exit", "code": 3}])
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.failed
    inventory(client, run_id)


@pytest.mark.parametrize("method,fields", [
    ("RunEnvironmentsRequest", {"limit": 0}),
    ("RunEnvironmentsRequest", {"limit": 501}),
    ("RunEnvironmentsRequest", {"offset": 2**64 - 1}),
    ("RunEnvironmentPathsRequest", {"environmentId": "missing", "limit": 0}),
    ("RunEnvironmentPathsRequest", {"environmentId": "missing", "offset": 2**64 - 1}),
])
def test_environment_queries_reject_invalid_ranges(client, method, fields):
    """Environment pages reject invalid limits and overflowing offsets."""
    with pytest.raises(RpcFailure) as error:
        client.call(method, runId=new_id(), **fields)
    assert error.value.kind == "invalid_request"


def test_unknown_environment_is_not_found(client):
    """A missing run or environment cannot expose another run's environment."""
    for method, fields in [("RunEnvironmentsRequest", {}),
                            ("RunEnvironmentPathsRequest", {"environmentId": "missing"})]:
        with pytest.raises(RpcFailure) as error:
            client.call(method, runId=new_id(), **fields)
        assert error.value.kind == "not_found"


def test_cards_share_one_environment_and_changed_builds_get_new_ids(core_factory, project, tmp_path):
    """Cards sharing a build share one inventory entry; a changed build gets a new ID."""
    package = tmp_path / "package"
    package.mkdir()
    (package / "scenario.json").write_text(json.dumps({"actions": []}))
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project).card("start", {"template": "trigger"}).card("split", {"template": "fork"})
    graph.connect("start", "out", "split", "in")
    for name in ["a", "b"]:
        graph.card(name, {"language": "bash", "sandbox": False, "inputs": [{"name": "start"}]},
                   outputs=STRING_OUTPUT, manifest={"run": {"package": str(package)}})
        graph.connect("split", "out1" if name == "a" else "out2", name, "start")
    graph.save(client)
    first = run(client, "start")
    assert wait_run(client, first).state == RunState.succeeded
    page = client.call("RunEnvironmentsRequest", runId=first)
    assert len(page.environments) == 1
    original_id = text(page.environments[0].id)
    latest = client.call("RunLatestInvocationsRequest", runId=first).records
    ids = {"/".join(text(x) for x in row.card.segments): text(row.environmentId) for row in latest}
    assert ids == {"start": None, "split": None, "a": original_id, "b": original_id}
    graph.card("b", {"language": "bash", "sandbox": False, "inputs": [{"name": "start"}]},
               outputs=STRING_OUTPUT, manifest={"run": {"package": str(package)}, "preRun": "true"})
    graph.save(client)
    second = run(client, "start")
    assert wait_run(client, second).state == RunState.succeeded
    page = client.call("RunEnvironmentsRequest", runId=second)
    assert len(page.environments) == 2
    first_page = client.call("RunEnvironmentsRequest", runId=second, limit=1)
    assert first_page.nextOffset == 1
    second_page = client.call("RunEnvironmentsRequest", runId=second, offset=first_page.nextOffset, limit=1)
    assert second_page.nextOffset is None
    assert {text(first_page.environments[0].id), text(second_page.environments[0].id)} == {text(e.id) for e in page.environments}
    rows = client.call("RunLatestInvocationsRequest", runId=second).records
    ids = {"/".join(text(x) for x in row.card.segments): text(row.environmentId) for row in rows}
    assert ids["a"] == original_id and ids["b"] != original_id
    assert len(client.call("RunEnvironmentsRequest", runId=first).environments) == 1


def test_build_failure_has_no_claimed_environment(core_factory, project):
    """A Card that never obtained a build does not claim an execution environment."""
    client = core_factory(mocked_builds=True).connect()
    Project(project).card("work", {"language": "bash"}, outputs=STRING_OUTPUT,
                          plan={"error": "could not build this environment"}).save(client)
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.failed
    assert client.call("RunEnvironmentsRequest", runId=run_id).environments == []
    assert client.call("RunRecordRequest", runId=run_id, card=path("work")).record.summary.environmentId is None


def test_deleting_history_removes_its_environment_inventory(core_factory, project, tmp_path):
    """Deleted run environments stay deleted after the project history is restored."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True, history_dir=tmp_path / "history")
    client = daemon.connect()
    worker_project(client, project, tmp_path, [])
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    environment_id, _ = inventory(client, run_id)
    client.call("RunHistoryDeleteRequest", opId=new_id())
    daemon.stop()
    copied_project = tmp_path / "copy"
    shutil.copytree(project, copied_project, symlinks=True)
    restored = core_factory(project_root=copied_project, history_dir=tmp_path / "restored-history")
    with pytest.raises(RpcFailure) as error:
        restored.connect().call("RunEnvironmentPathsRequest", runId=run_id, environmentId=environment_id)
    assert error.value.kind == "not_found"



def test_renamed_card_keeps_its_recorded_environment(core_factory, project, tmp_path):
    """Renaming a Card preserves its environment and portable invocation history."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True, history_dir=tmp_path / "history")
    client = daemon.connect()
    worker_project(client, project, tmp_path, [])
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    expected = inventory(client, run_id)
    snapshot = client.call("ProjectInspectRequest")
    document = json.loads(text(snapshot.projectJson))
    document["canvas"]["cards"][0]["id"] = "renamed"
    client.call("ProjectSaveRequest", baseRev=snapshot.rev, opId=new_id(), origin="ui:test",
                changes=[change("SetProject", json=json.dumps(document)),
                         change("RenameDefinition", id="work", to="renamed")])
    assert inventory(client, run_id, "renamed") == expected
    daemon.stop()
    copied_project = tmp_path / "copy"
    shutil.copytree(project, copied_project, symlinks=True)
    restored = core_factory(project_root=copied_project, history_dir=tmp_path / "restored-history")
    assert inventory(restored.connect(), run_id, "renamed") == expected


def test_shipped_cli_can_inspect_the_environment_inventory(core_factory, project, tmp_path):
    """The shipped C++ CLI lists recorded environments without a separate history client."""
    import os
    import subprocess
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path, [])
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    environment_id, _ = inventory(client, run_id)
    requests = [
        ("SessionHelloRequest", {"protocol": 1, "client_id": new_id()}),
        ("RunEnvironmentsRequest", {"run_id": run_id}),
        ("RunEnvironmentPathsRequest", {"run_id": run_id, "environment_id": environment_id, "limit": 1}),
    ]
    lines = "".join(json.dumps({"request_id": index, "payload_type": "Request",
                               "payload": {"value_type": method, "value": fields}}) + "\n"
                    for index, (method, fields) in enumerate(requests, 1))
    result = subprocess.run([os.environ["TSUGUMI_RPC_CLIENT"], "--endpoint", "unix:" + daemon.unix,
                             "--schema", os.environ["TSUGUMI_PROJECT_SCHEMA"]],
                            input=lines, text=True, capture_output=True, check=True, timeout=10)
    replies = {value["request_id"]: value["payload"] for value in map(json.loads, result.stdout.splitlines())}
    assert replies[2]["value_type"] == "BuildEnvironmentPage"
    assert replies[2]["value"]["environments"][0]["id"] == environment_id
    assert replies[3]["value_type"] == "EnvironmentPathPage"
    assert replies[3]["value"]["next_offset"] == 1


def test_interrupted_execution_keeps_its_environment_after_a_crash(core_factory, project, tmp_path):
    """Crash recovery preserves the environment associated before a worker began executing."""
    from harness.scenario import Gate
    daemon = core_factory(mocked_builds=True, mocked_worker=True, history_dir=tmp_path / "history")
    client = daemon.connect()
    gate = Gate(tmp_path, "executing")
    worker_project(client, project, tmp_path, [gate.action()])
    run_id = run(client)
    gate.wait()
    rows = client.call("RunLatestInvocationsRequest", runId=run_id).records
    environment_id = text(rows[0].environmentId)
    assert environment_id
    daemon.stop(crash=True)
    daemon.start()
    client = daemon.connect()
    status = client.call("RunStatusRequest", runId=run_id)
    assert status.state == RunState.failed
    assert inventory(client, run_id)[0] == environment_id
    daemon.stop()
    copied_project = tmp_path / "copy"
    shutil.copytree(project, copied_project, symlinks=True)
    restored = core_factory(project_root=copied_project, history_dir=tmp_path / "restored-history")
    assert inventory(restored.connect(), run_id)[0] == environment_id


def test_dependency_search_is_literal_case_insensitive_and_paginated(core_factory, project, tmp_path):
    """Search covers the recorded closure, with offsets and totals over matching paths."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    worker_project(client, project, tmp_path, [])
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    environment_id, paths = inventory(client, run_id)
    for query in ["", "NIX", paths[-1].split("/")[-1], "%", "_", "'", "no-such-package"]:
        expected = [p for p in paths if query.lower() in p.lower()]
        found = []
        offset = 0
        while True:
            page = client.call("RunEnvironmentPathsRequest", runId=run_id,
                               environmentId=environment_id, query=query, offset=offset, limit=1)
            assert page.total == len(expected)
            found.extend(text(p) for p in page.paths)
            if page.nextOffset is None:
                break
            assert page.nextOffset > offset
            offset = page.nextOffset
        assert found == expected


def test_environment_diff_is_paginated_and_validates_both_sides(core_factory, project, tmp_path):
    """Comparisons return exact set differences; missing history is never an empty closure."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    worker_project(client, project, tmp_path, [])
    first = run(client)
    assert wait_run(client, first).state == RunState.succeeded
    old_id, old_paths = inventory(client, first)
    # Change the build product through the normal project source path.
    package = tmp_path / "changed-worker"
    package.mkdir()
    (package / "scenario.json").write_text(json.dumps({"actions": []}))
    graph = Project(project).card("changed", {"language": "bash", "sandbox": False},
                                  outputs=STRING_OUTPUT, manifest={"run": {"package": str(package)}})
    graph.save(client)
    second = run(client, "changed")
    assert wait_run(client, second).state == RunState.succeeded
    changed_id, new_paths = inventory(client, second, "changed")
    for left, left_id, left_paths, right, right_id, right_paths in [
        (first, old_id, old_paths, second, changed_id, new_paths),
        (second, changed_id, new_paths, first, old_id, old_paths),
        (first, old_id, old_paths, first, old_id, old_paths),
    ]:
        expected = sorted(set(left_paths) - set(right_paths))
        found, offset = [], 0
        while True:
            page = client.call("RunEnvironmentPathsRequest", runId=left, environmentId=left_id,
                               excludeRunId=right, excludeEnvironmentId=right_id, limit=1, offset=offset)
            assert page.total == len(expected)
            found.extend(text(p) for p in page.paths)
            if page.nextOffset is None:
                break
            assert page.nextOffset > offset
            offset = page.nextOffset
        assert found == expected
    for fields, kind in [({"excludeRunId": first}, "invalid_request"),
                         ({"excludeEnvironmentId": old_id}, "invalid_request"),
                         ({"excludeRunId": first, "excludeEnvironmentId": "missing"}, "not_found"),
                         ({"excludeRunId": new_id(), "excludeEnvironmentId": old_id}, "not_found")]:
        with pytest.raises(RpcFailure) as error:
            client.call("RunEnvironmentPathsRequest", runId=first, environmentId=old_id, **fields)
        assert error.value.kind == kind
