"""Public RPC coverage for Card evaluation, project locks and generated schemas."""

import json

import pytest

from harness.definition_evaluation import EMPTY_OUTPUTS, reload_project
from harness.scenario import Project
from tsugumi_client import path, text


OUTPUTS = "table Value { text:string; } table Outputs { out:Value; } root_type Outputs;"


def evaluated(client, card, expression, **options):
    result = client.call("EvaluateNixRequest", card=path(card), expression=expression, **options)
    assert not result.failure, result.failure and text(result.failure.diagnostic)
    return json.loads(text(result.json))


@pytest.mark.environment
def test_nix_inputs_are_transitive_exports_and_stop_at_merge(client, project):
    graph = Project(project)
    graph.card("A", {"type": "code"}, outputs=OUTPUTS, sources={
        "default.nix": '{ params, inputs }: { exports.message = params.text; }',
        "params.nix": '{ text = "from A"; }',
    })
    graph.card("B", {"type": "code", "inputs": [{"name": "in"}]}, outputs=OUTPUTS, sources={
        "default.nix": '{ params, inputs }: { exports.message = inputs.A.message + " via B"; }',
    })
    for name in ("C", "D"):
        graph.card(name, {"type": "code", "inputs": [{"name": "in"}]}, outputs=EMPTY_OUTPUTS)
    graph.card("merge", {"type": "merge", "inputs": [{"name": "a"}]})
    graph.connect("A", "out", "B", "in").connect("B", "out", "C", "in")
    graph.connect("A", "out", "merge", "a").connect("merge", "out", "D", "in")
    graph.save(client)
    assert evaluated(client, "C", "inputs") == {
        "A": {"message": "from A"}, "B": {"message": "from A via B"},
    }
    assert evaluated(client, "D", "inputs") == {}
    assert evaluated(client, "C", "params") == {}
    assert evaluated(client, "C", "null", build=True) is None


def test_generated_inputs_refresh_on_change_and_keep_their_mtime_on_identical_types(client, project):
    source = project / "definitions/A/_outputs.fbs"
    target = project / "definitions/B/_inputs.fbs"
    graph = Project(project).card("A", {"type": "code"}, outputs=OUTPUTS)
    graph.card("B", {"type": "code", "inputs": [{"name": "wired"}, {"name": "unused"}]},
               outputs=EMPTY_OUTPUTS)
    graph.connect("A", "out", "B", "wired").save(client)
    original, mtime = target.read_text(), target.stat().st_mtime_ns
    assert "wired: inputs.wired.Value" in original
    assert "unused:" not in original
    source.write_text("// comment only\n" + OUTPUTS)
    reload_project(client)
    assert target.read_text() == original
    assert target.stat().st_mtime_ns == mtime
    source.write_text(OUTPUTS.replace("text:string", "count:int"))
    reload_project(client)
    changed = target.read_text()
    assert changed != original and "count:int" in changed

    graph.cards["A"][1]["_outputs.fbs"] = source.read_text()
    graph.connections.clear()
    graph.save(client)
    assert not target.exists()


@pytest.mark.environment
def test_sdk_build_handles_missing_api_and_rejects_foreign_systems(client, project):
    graph = Project(project)
    graph.card("no-api", {"type": "code"}, outputs=EMPTY_OUTPUTS, sources={
        "default.nix": "{ params, inputs }: { run = []; }",
    })
    graph.card("foreign", {"type": "code"}, outputs=EMPTY_OUTPUTS, sources={
        "default.nix": '''{ params, inputs }: {
          api = builtins.derivation {
            name = "foreign-api";
            system = "tsugumi-test-foreign-system";
            builder = "/bin/sh";
          };
        }''',
    })
    graph.save(client)
    expression = "(import ./. { inherit params inputs; }).api or null"
    assert evaluated(client, "no-api", expression, build=True) is None
    result = client.call("EvaluateNixRequest", card=path("foreign"),
                         expression=expression, build=True)
    assert result.failure
    assert "Nix expression must build for" in text(result.failure.diagnostic)
