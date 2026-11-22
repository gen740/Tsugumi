"""Inspect recorded build environments through public RPC, without re-evaluation."""

import json
import shutil
import sqlite3
from pathlib import Path

import pytest
from harness.scenario import STRING_OUTPUT, Project, run, wait_run
from test_run_lists import mock_plan, scripted_implementations
from Tsugumi.Data.RunState import RunState
from tsugumi_client import RpcFailure, new_id, path, text


def worker_project(client, project, directory, actions):
    Project(project).card(
        "work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": True},
        outputs=STRING_OUTPUT, plan=mock_plan(scripted_implementations(directory, [actions])),
    ).save(client)


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
    assert len(paths) == len(set(paths))
    assert text(environment.manifest) in paths
    assert text(environment.derivation).endswith(".drv")
    users = client.call("RunEnvironmentUsesRequest", runId=run_id, environmentId=text(environment.id), limit=1)
    assert [(text(u.card), u.invocation) for u in users.uses] == [(card, 1)]
    assert users.nextOffset is None
    return text(environment.id), set(paths)


def test_environment_survives_cache_reuse_restart_and_project_copy(core_factory, project, tmp_path):
    """README edits preserve the recorded builds and cache URLs in a copied Project."""
    listed = tmp_path / "substituters"
    listed.write_text("https://cache.nixos.org\n"
                      "https://reader:cache-secret@private.example.org/team?token=cache-token\n"
                      "\n  \nhttps://cache.nixos.org\n"
                      "s3://bucket/cache?profile=private-profile\n"
                      "file:///private-cache-dir\n")
    expected = ["https://cache.nixos.org", "https://private.example.org/team", "s3://bucket/cache"]
    daemon = core_factory(mocked_builds=True, mocked_worker=True,
                          args=["--substituters-file", str(listed)])
    client = daemon.connect()
    worker_project(client, project, tmp_path, [])
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    cached = run(client)
    assert wait_run(client, cached).state == RunState.succeeded
    environment = inventory(client, run_id)
    assert inventory(client, cached) == environment
    from test_definitions import edit_description
    edit_description(client, "work", "Updated description")
    daemon.stop()
    # The list is the run's own: a later configuration does not rewrite it,
    # and it travels with the project's copy of the run.
    listed.write_text("https://later.example.org\n")
    copied_project = tmp_path / "copy"
    shutil.copytree(project, copied_project, symlinks=True)
    restored = core_factory(project_root=copied_project, mocked_builds=True, mocked_worker=True,
                            args=["--substituters-file", str(listed)]).connect()
    assert inventory(restored, run_id) == inventory(restored, cached) == environment
    page = restored.call("RunEnvironmentsRequest", runId=run_id)
    assert [text(url) for url in page.substituters] == expected
    later = run(restored)
    assert wait_run(restored, later).state == RunState.succeeded
    page = restored.call("RunEnvironmentsRequest", runId=later)
    assert [text(url) for url in page.substituters] == ["https://later.example.org"]
    for folder in (project, copied_project, tmp_path / "history", tmp_path / "restored-history"):
        for file in folder.rglob("*"):
            if file.is_file():
                data = file.read_bytes()
                for secret in (b"cache-secret", b"cache-token", b"private-profile", b"reader:", b"private-cache-dir"):
                    assert secret not in data, file


def test_a_run_without_a_cache_list_records_none(core_factory, project, tmp_path):
    """A daemon that knows no binary caches reports an empty list, not an error."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True,
                          args=["--substituters-file", str(tmp_path / "missing")])
    client = daemon.connect()
    worker_project(client, project, tmp_path, [])
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    assert client.call("RunEnvironmentsRequest", runId=run_id).substituters == []


def test_failed_execution_keeps_its_environment(core_factory, project, tmp_path):
    """A worker failure still records the environment that was used."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path, [{"op": "exit", "code": 3}])
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.failed
    inventory(client, run_id)


def test_environment_queries_reject_invalid_ranges(client):
    """Environment pages reject invalid limits, overflowing offsets and partial exclusions."""
    for method, fields in (
        ("RunEnvironmentsRequest", {"limit": 0}),
        ("RunEnvironmentsRequest", {"limit": 501}),
        ("RunEnvironmentsRequest", {"offset": 2**64 - 1}),
        ("RunEnvironmentPathsRequest", {"environmentId": "missing", "limit": 0}),
        ("RunEnvironmentPathsRequest", {"environmentId": "missing", "offset": 2**64 - 1}),
        ("RunEnvironmentPathsRequest", {"environmentId": "missing", "excludingRunId": new_id()}),
        ("RunEnvironmentPathsRequest", {"environmentId": "missing", "excludingEnvironmentId": "other"}),
        ("RunEnvironmentUsesRequest", {"environmentId": "missing", "limit": 0}),
        ("RunEnvironmentUsesRequest", {"environmentId": "missing", "offset": 2**64 - 1}),
    ):
        with pytest.raises(RpcFailure) as error:
            client.call(method, runId="999999", **fields)
        assert error.value.kind == "invalid_request"


def test_unknown_environment_is_not_found(client):
    """A missing run or environment cannot expose another run's environment."""
    for method, fields in [("RunEnvironmentsRequest", {}),
                            ("RunEnvironmentPathsRequest", {"environmentId": "missing"}),
                            ("RunEnvironmentUsesRequest", {"environmentId": "missing"})]:
        with pytest.raises(RpcFailure) as error:
            client.call(method, runId="999999", **fields)
        assert error.value.kind == "not_found"


def test_cards_share_one_environment_and_changed_builds_get_separate_entries(core_factory, project, tmp_path):
    """Cards sharing a build share one run-local inventory entry."""
    package = tmp_path / "package"
    package.mkdir()
    (package / "scenario.json").write_text(json.dumps({"actions": []}))
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    graph = Project(project).card("start", {"type": "trigger"})
    for name in ["a", "b"]:
        graph.card(name, {"type": "code", "sandbox": {"filesystem": False}, "inputs": [{"name": "start"}]},
                   outputs=STRING_OUTPUT, plan=mock_plan([str(package)]))
        graph.connect("start", "out", name, "start")
    graph.save(client)
    first = run(client, "start")
    assert wait_run(client, first).state == RunState.succeeded
    page = client.call("RunEnvironmentsRequest", runId=first)
    assert len(page.environments) == 1
    original_environment = (text(page.environments[0].derivation), text(page.environments[0].manifest))
    original_id = text(page.environments[0].id)
    users = client.call("RunEnvironmentUsesRequest", runId=first, environmentId=original_id, limit=1)
    assert [(text(u.card), u.invocation) for u in users.uses] == [("a", 1)]
    assert users.nextOffset == 1
    users = client.call("RunEnvironmentUsesRequest", runId=first, environmentId=original_id,
                        offset=users.nextOffset, limit=1)
    assert [(text(u.card), u.invocation) for u in users.uses] == [("b", 1)]
    assert users.nextOffset is None
    latest = client.call("RunLatestInvocationsRequest", runId=first).records
    ids = {"/".join(text(x) for x in row.card.segments): text(row.environmentId) for row in latest}
    assert ids == {"start": None, "a": original_id, "b": original_id}
    changed, = scripted_implementations(tmp_path / "changed", [[]])
    graph.card("b", {"type": "code", "sandbox": {"filesystem": False}, "inputs": [{"name": "start"}]},
               outputs=STRING_OUTPUT, plan=mock_plan([changed]))
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
    assert ids["a"] != ids["b"]
    environments = {text(e.id): (text(e.derivation), text(e.manifest)) for e in page.environments}
    assert environments[ids["a"]] == original_environment
    assert environments[ids["b"]] != original_environment
    assert len(client.call("RunEnvironmentsRequest", runId=first).environments) == 1
    changed_id = ids["b"]
    def closure(run_id, environment_id):
        return {text(p) for p in client.call("RunEnvironmentPathsRequest", runId=run_id,
                                           environmentId=environment_id).paths}
    old_paths, new_paths = closure(first, original_id), closure(second, changed_id)
    for run_id, environment_id, other_run, other_id, expected in [
        (first, original_id, second, changed_id, old_paths - new_paths),
        (second, changed_id, first, original_id, new_paths - old_paths),
        (first, original_id, second, ids["a"], []),
    ]:
        for query in ["", "NIX", "%", "no-such-dependency"]:
            matching = [p for p in expected if query.lower() in p.lower()]
            offset, found = 0, []
            while True:
                page = client.call("RunEnvironmentPathsRequest", runId=run_id, environmentId=environment_id,
                                   excludingRunId=other_run, excludingEnvironmentId=other_id,
                                   query=query, offset=offset, limit=1)
                assert page.total == len(matching)
                found.extend(text(p) for p in page.paths)
                if page.nextOffset is None:
                    break
                assert page.nextOffset > offset
                offset = page.nextOffset
            assert len(found) == len(matching)
            assert set(found) == set(matching)
    with pytest.raises(RpcFailure) as error:
        client.call("RunEnvironmentPathsRequest", runId=first, environmentId=original_id,
                    excludingRunId=second, excludingEnvironmentId="missing")
    assert error.value.kind == "not_found"


@pytest.mark.parametrize(("method", "statement", "needs_environment"), [
    ("RunEnvironmentsRequest",
     "UPDATE build_environments SET manifest=? WHERE run_id=?", False),
    ("RunEnvironmentPathsRequest",
     "UPDATE environment_paths SET path=? WHERE run_id=?", True),
    ("RunEnvironmentUsesRequest",
     "UPDATE invocations SET card=? WHERE run_id=?", True),
])
def test_oversized_history_rows_fail_instead_of_returning_a_stalled_cursor(
        core_factory, project, tmp_path, method, statement, needs_environment):
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    worker_project(client, project, tmp_path, [])
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    environment_id, _ = inventory(client, run_id)
    daemon.stop()

    # Simulate an oversized row in recorded history. It must fail even when
    # the first row alone exceeds the response budget.
    db = sqlite3.connect(project / ".tsugumi" / "runs.sqlite")
    try:
        with db:
            updated = db.execute(statement, ("/nix/store/" + "x" * (2 << 20), run_id))
            assert updated.rowcount
    finally:
        db.close()
    daemon.start()
    fields = {"environmentId": environment_id} if needs_environment else {}
    with pytest.raises(RpcFailure) as error:
        daemon.connect().call(method, runId=run_id, **fields)
    assert error.value.kind == "resource_limit"


def test_build_failure_has_no_claimed_environment(core_factory, project):
    """A Card that never obtained a build does not claim an execution environment."""
    client = core_factory(mocked_builds=True).connect()
    Project(project).card("work", {"type": "code"}, outputs=STRING_OUTPUT,
                          plan={"error": "could not build this environment"}).save(client)
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.failed
    assert client.call("RunEnvironmentsRequest", runId=run_id).environments == []
    assert client.call("RunRecordRequest", runId=run_id, card=path("work")).record.summary.environmentId is None


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
        ("SessionHelloRequest", {}),
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
    """Crash recovery preserves the environment and service of an interrupted execution."""
    from harness.scenario import Gate
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "executing")
    worker_project(client, project, tmp_path, [gate.action()])
    run_id = run(client)
    gate.wait()
    rows = client.call("RunLatestInvocationsRequest", runId=run_id).records
    environment_id = text(rows[0].environmentId)
    assert environment_id
    assert text(rows[0].executionService) == "local"
    daemon.stop(crash=True)
    daemon.start()
    client = daemon.connect()
    status = client.call("RunStatusRequest", runId=run_id)
    assert status.state == RunState.failed
    assert inventory(client, run_id)[0] == environment_id
    rows = client.call("RunLatestInvocationsRequest", runId=run_id).records
    assert text(rows[0].executionService) == "local"
    daemon.stop()
    copied_project = tmp_path / "copy"
    shutil.copytree(project, copied_project, symlinks=True)
    restored = core_factory(project_root=copied_project)
    copied_client = restored.connect()
    assert inventory(copied_client, run_id)[0] == environment_id
    rows = copied_client.call("RunLatestInvocationsRequest", runId=run_id).records
    assert text(rows[0].executionService) == "local"


def test_dependency_search_is_literal_case_insensitive_and_paginated(core_factory, project, tmp_path):
    """Search covers the recorded closure, with offsets and totals over matching paths."""
    client = core_factory(mocked_builds=True, mocked_worker=True).connect()
    worker_project(client, project, tmp_path, [])
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    environment_id, paths = inventory(client, run_id)
    for query in ["", "NIX", max(paths).rsplit("/", 1)[-1], "%", "_", "'", "no-such-package"]:
        expected = {p for p in paths if query.lower() in p.lower()}
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
        assert len(found) == len(expected)
        assert set(found) == expected


def test_local_worker_uses_the_recorded_build_plan(core_factory, project, tmp_path):
    """Each ABI 9 package and its declared view are in the recorded closure."""
    from harness.daemon import ROOT
    from harness.native_card import STATIC_RESULT_ENTRY, card_source
    from harness.observations import decode_scalar, read_output
    from harness.values import encode_message
    from Tsugumi.Data.PortKind import PortKind

    client = core_factory().connect()
    graph = Project(project)
    inventories = []
    abi_header = (ROOT / "protocol/include/tsugumi/card_abi.h").read_text()
    for version in ["first", "changed"]:
        message = encode_message(tmp_path / "encode", STRING_OUTPUT, {"value": version},
                                 root_type="Value")
        sources = {
            "tsugumi/card_abi.h": abi_header,
            "main.c": card_source(STATIC_RESULT_ENTRY, [("out", PortKind.FlatBuffer, message)]),
            "view.html": "<p>Version " + version + "</p>",
            "default.nix": """{ pkgs, ... }: {
              run = pkgs.runCommandCC "recorded-plan-card" { } ''
                mkdir -p "$out/lib"
                "$CC" -shared -fPIC -I${./.} ${./main.c} -o "$out/lib/main.so"
              '';
              views = [{ name = "view"; type = "html"; blob = ./view.html; }];
            }""",
        }
        graph.card("work", {"type": "code", "sandbox": {"filesystem": False}, "recordIO": True,
                            "reuseWorker": True}, outputs=STRING_OUTPUT, sources=sources).save(client)
        for _ in range(2):
            run_id = run(client)
            assert wait_run(client, run_id).state == RunState.succeeded
            assert decode_scalar(read_output(client, run_id)) == version
            _, paths = inventory(client, run_id)
            environment, = client.call("RunEnvironmentsRequest", runId=run_id).environments
            plan = json.loads(Path(text(environment.manifest)).read_text())
            view, = plan["views"]
            assert Path(view["blob"]).read_text() == sources["view.html"]
            assert {*plan["runs"], view["blob"]} <= paths
            inventories.append(paths)
    assert inventories[0] == inventories[1]
    assert inventories[2] == inventories[3]
    assert inventories[0] != inventories[2]
