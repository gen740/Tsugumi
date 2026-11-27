"""Public RPC coverage for Card evaluation, project locks and generated schemas."""

import json

import pytest

from harness.definition_evaluation import EMPTY_OUTPUTS, reload_project
from harness.scenario import Project
from tsugumi_client import RpcFailure, path, text


OUTPUTS = "table Value { text:string; } table Outputs { out:Value; } root_type Outputs;"


def evaluated(client, card, expression, **options):
    result = client.call("EvaluateNixRequest", card=path(card), expression=expression, **options)
    assert not result.failure, result.failure and text(result.failure.diagnostic)
    return json.loads(text(result.json))


@pytest.mark.environment
def test_evaluation_reads_only_confirmed_definition_sources(client, project):
    Project(project).card("work", {"type": "code"}, outputs=EMPTY_OUTPUTS, sources={
        "default.nix": "{ params, inputs }: {}",
        "message.txt": "confirmed",
    }).save(client)
    definition = project / "definitions/work"
    (definition / "_comments.json").write_text('{"comments":[]}')
    reload_project(client)
    names = evaluated(client, "work", "builtins.attrNames (builtins.readDir ./.)")
    assert {"default.nix", "message.txt", "_inputs.fbs"} <= set(names)
    metadata = {"README.md", "_card.json", "_comments.json"}
    assert all((definition / name).is_file() for name in metadata)
    assert metadata.isdisjoint(names)
    assert evaluated(client, "work", "builtins.pathExists ./_comments.json") is False
    packaged = evaluated(client, "work", """
      let source = builtins.path { path = ./.; name = "confirmed-definition"; };
      in builtins.attrNames (builtins.readDir source)
    """)
    assert packaged == names
    assert evaluated(client, "work", "builtins.readFile ./message.txt") == "confirmed"
    generated = (definition / "_inputs.fbs").read_text()
    (definition / "_inputs.fbs").unlink()
    assert evaluated(client, "work", "builtins.readFile ./_inputs.fbs") == generated
    assert not (definition / "_inputs.fbs").exists()

    (definition / "unconfirmed.txt").write_text("external")
    with pytest.raises(RpcFailure) as failure:
        evaluated(client, "work", "builtins.attrNames (builtins.readDir ./.)")
    assert failure.value.kind == "external_change"
    (definition / "unconfirmed.txt").unlink()

    (definition / "message.txt").write_text("external")
    with pytest.raises(RpcFailure) as failure:
        evaluated(client, "work", "builtins.readFile ./message.txt")
    assert failure.value.kind == "external_change"
    reload_project(client)
    assert evaluated(client, "work", "builtins.readFile ./message.txt") == "external"


@pytest.mark.environment
def test_nix_inputs_are_transitive_exports_and_merge_passes_on_what_its_inputs_share(client, project):
    graph = Project(project)
    graph.card("a", {"type": "trigger"}, sources={
        "default.nix": '{ params, inputs }: { exports.message = params.text; }',
        "params.nix": '{ text = "from A"; }',
    })
    graph.card("b", {"type": "code", "inputs": [{"name": "in"}]}, outputs=OUTPUTS, sources={
        "default.nix": '{ params, inputs }: { exports.message = inputs.a.message + " via B"; }',
    })
    for name in ("c", "d"):
        graph.card(name, {"type": "code", "inputs": [{"name": "in"}]}, outputs=EMPTY_OUTPUTS)
    graph.card("merge", {"type": "merge", "inputs": [{"name": "a"}]})
    graph.connect("a", "out", "b", "in").connect("b", "out", "c", "in")
    graph.connect("a", "out", "merge", "a").connect("merge", "out", "d", "in")
    graph.save(client)
    assert evaluated(client, "c", "inputs") == {
        "a": {"message": "from A"}, "b": {"message": "from A via B"},
    }
    # The Merge's one input exposes a, so a is common to all its inputs.
    assert evaluated(client, "d", "inputs") == {"a": {"message": "from A"}}
    assert evaluated(client, "c", "params") == {}
    assert evaluated(client, "c", "null", build=True) is None


def test_generated_inputs_refresh_on_change_and_keep_their_mtime_on_identical_types(client, project):
    source = project / "definitions/a/_outputs.fbs"
    target = project / "definitions/b/_inputs.fbs"
    graph = Project(project).card("a", {"type": "code"}, outputs=OUTPUTS)
    graph.card("b", {"type": "code", "inputs": [{"name": "wired"}, {"name": "unused"}]},
               outputs=EMPTY_OUTPUTS)
    graph.connect("a", "out", "b", "wired").save(client)
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

    graph.cards["a"][1]["_outputs.fbs"] = source.read_text()
    graph.connections.clear()
    graph.save(client)
    assert target.read_text().endswith("namespace inputs;\ntable Inputs {\n}\nroot_type Inputs;\n")


@pytest.mark.environment
def test_sdk_build_handles_missing_api_and_rejects_foreign_systems(client, project):
    graph = Project(project)
    graph.card("no_api", {"type": "code"}, outputs=EMPTY_OUTPUTS, sources={
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
    assert evaluated(client, "no_api", expression, build=True) is None
    result = client.call("EvaluateNixRequest", card=path("foreign"),
                         expression=expression, build=True)
    assert result.failure
    assert "Nix expression must build for" in text(result.failure.diagnostic)
