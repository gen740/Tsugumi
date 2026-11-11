"""Recorded environments describe the actual Nix output, including cache hits."""

import subprocess

from harness.scenario import Project, run, wait_run
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState


def test_recorded_closure_matches_nix_and_survives_definition_changes(core_factory, project):
    """History records Nix's actual runtime closure and keeps earlier environments unchanged."""
    client = core_factory().connect()
    graph = Project(project)
    environments = []
    for greeting in ["first", "first", "changed"]:
        graph.card("work", {"language": "bash", "sandbox": False},
                   outputs="table Outputs {} root_type Outputs;",
                   sources={"default.nix": '{ pkgs, inputs, params }: { run = "echo ' + greeting + '"; }'})
        graph.save(client)
        run_id = run(client)
        status = wait_run(client, run_id, timeout=300)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        environment = client.call("RunEnvironmentsRequest", runId=run_id).environments[0]
        assert text(client.call("RunRecordRequest", runId=run_id, card=path("work")).record.summary.environmentId) == text(environment.id)
        actual = subprocess.check_output(["nix-store", "--query", "--requisites", text(environment.manifest)], text=True)
        deriver = subprocess.check_output(["nix-store", "--query", "--deriver", text(environment.manifest)], text=True).strip()
        assert text(environment.derivation) == deriver
        assert text(environment.system)
        paths, offset = [], 0
        while True:
            page = client.call("RunEnvironmentPathsRequest", runId=run_id,
                               environmentId=text(environment.id), offset=offset, limit=3)
            paths.extend(text(value) for value in page.paths)
            if page.nextOffset is None:
                break
            offset = page.nextOffset
        assert paths == sorted(actual.splitlines())
        assert environment.pathCount == len(paths)
        environments.append((run_id, text(environment.id), paths))
    assert environments[0][1:] == environments[1][1:]
    assert environments[0][1] != environments[2][1]
    assert text(client.call("RunEnvironmentsRequest", runId=environments[0][0]).environments[0].id) == environments[0][1]
