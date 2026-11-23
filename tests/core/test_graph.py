"""Port resolution, Nix references and run plans through public RPC."""

import pytest

from projects import card, card_in_card, conn, write
from tsugumi_client import change, new_id, path, text


NUMBER = 'namespace t; table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;'
FLAG = 'namespace t; table Flag { value:bool; } table Outputs { flag:Flag (required); } root_type Outputs;'


def code(inputs=(), *, outputs=NUMBER, body='{ run = "true"; }'):
    return {
        "type": "code", "sandbox": {"filesystem": False},
        "inputs": [{"name": name} for name in inputs],
        "_outputs": outputs, "_nix": "{ params, inputs }: " + body,
    }


def inspect_graph(core_factory, project, definitions, connections, cards=None):
    write(project, definitions, cards or [card(name, name) for name in definitions], connections)
    client = core_factory(project_root=project).connect()
    resolved = {"/".join(text(segment) for segment in node.card.segments): node
                for node in client.call("ProjectInspectRequest").resolution}
    return client, resolved


def codes(node):
    return {text(diagnostic.code) for diagnostic in node.diagnostics}


@pytest.mark.parametrize("destination", ["right", "left"], ids=["distinct-input", "duplicate-wire"])
def test_one_output_feeds_distinct_inputs_once(core_factory, project, destination):
    """One output can feed several ports; the same endpoints cannot be wired twice."""
    _, resolved = inspect_graph(core_factory, project,
                                {"source": code(), "sink": code(["left", "right"])},
                                [conn("source", "out", "sink", "left"),
                                 conn("source", "out", "sink", destination)])

    assert ("input_already_connected" in codes(resolved["sink"])) == (destination == "left")
    if destination == "right":
        assert {text(p.name): text(p.rootType) for p in resolved["sink"].inputs} == {
            "left": "t.Number", "right": "t.Number",
        }


@pytest.mark.parametrize("second", [NUMBER, FLAG], ids=["same-type", "different-type"])
def test_second_source_is_a_wiring_error_regardless_of_type(core_factory, project, second):
    _, resolved = inspect_graph(core_factory, project, {
        "one": code(), "two": code(outputs=second), "sink": code(["in", "unconnected"]),
    }, [conn("one", "out", "sink", "in"),
        conn("two", "out" if second == NUMBER else "flag", "sink", "in")])

    sink = resolved["sink"]
    assert codes(sink) == {"input_already_connected"}
    assert "Merge Card" in text(sink.diagnostics[0].message)
    assert text(sink.inputs[0].rootType) == "t.Number"
    assert sink.inputs[1].rootType is None


def test_merge_reports_distinct_member_names_on_inherited_ports(core_factory, project):
    _, resolved = inspect_graph(core_factory, project, {
        "one": code(), "duplicate": code(), "two": code(outputs=FLAG),
        "merge": {"type": "merge", "inputs": [{"name": n} for n in ("a", "b", "c", "unused")]},
        "relay": {"type": "merge", "inputs": [{"name": "in"}]}, "sink": code(["in"]),
    }, [conn("one", "out", "merge", "a"), conn("duplicate", "out", "merge", "b"),
        conn("two", "flag", "merge", "c"), conn("merge", "out", "relay", "in"),
        conn("relay", "out", "sink", "in")])

    for port in [resolved["merge"].outputs[0], resolved["relay"].outputs[0], resolved["sink"].inputs[0]]:
        assert [text(name) for name in port.mergeMemberTypes] == ["Flag", "Number"]
        assert not port.rootType


@pytest.mark.parametrize("schema", [
    "namespace t; table Value { value:int; }",
    'table Outputs { value:int; } root_type Outputs;',
], ids=["missing-output-catalog", "non-table-output"])
def test_invalid_output_types_are_reported_to_the_client(core_factory, project, schema):
    """A Card with an invalid output catalog exposes a diagnostic to its client."""
    _, resolved = inspect_graph(core_factory, project, {"source": code(outputs=schema)}, [])
    assert "invalid_outputs" in codes(resolved["source"])


def test_nested_cards_keep_their_types_and_run_boundaries(core_factory, project):
    """Typed values follow the flattened Workflow boundary."""
    definitions = {"start": {"type": "trigger"}, "source": code(),
                   "inner": code(["in"], body='{ run = inputs.in.command; }'),
                   "sink": code(["in"], body='{ run = inputs.in.command; }')}
    cards = [card("start", "start"), card("source", "source"),
             card_in_card("child", [card("inner", "inner")], [],
                          {"inputs": {"in": {"card": "inner", "port": "in"}},
                           "outputs": {"out": {"card": "inner", "port": "out"}}}), card("sink", "sink")]
    client, resolved = inspect_graph(core_factory, project, definitions,
                                     [conn("source", "out", "child", "in"), conn("child", "out", "sink", "in")], cards)
    assert not codes(resolved["sink"])
    assert not codes(resolved["child/inner"])
    assert not resolved["sink"].nixReferences
    assert not resolved["child/inner"].nixReferences
    assert text(resolved["sink"].inputs[0].rootType) == "t.Number"
    for target, propagate, expected in [
        ("start", True, {"start", "source", "child/inner", "sink"}),
        ("sink", False, {"sink"}), ("child", False, {"child/inner"}),
        ("child", True, {"child/inner", "sink"}),
    ]:
        plan = client.call("RunPlanRequest", card=path(target), propagate=propagate)
        assert {"/".join(text(segment) for segment in target.segments) for target in plan.targets} == expected


def test_nix_reference_inventory_depends_on_wires_not_source_text(core_factory, project):
    client, resolved = inspect_graph(core_factory, project,
                                    {"source": code(), "result": code(["data"], body='{ run = inputs.source.command; }')},
                                    [conn("source", "out", "result", "data")])

    def assert_reference(node):
        reference, = node.nixReferences
        assert text(reference.name) == "source"
        assert [text(segment) for segment in reference.target.segments] == ["source"]

    assert_reference(resolved["result"])
    for body in [
        '{ run = inputs.source.command; exports.again = inputs.source; }',
        '{ run = inputs.ghost.command; }',
        '{ /* inputs.source */ run = "true"; exports.note = "inputs.ghost"; }',
    ]:
        snap = client.call("ProjectInspectRequest")
        client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="test", changes=[
            change("PutSource", path="definitions/result/default.nix",
                   baseHash=next(text(s.hash) for s in snap.sources if text(s.path) == "definitions/result/default.nix"),
                   bytes=list(('{ inputs, params }: ' + body).encode()))])
        snap = client.call("ProjectInspectRequest")
        result = next(r for r in snap.resolution if [text(s) for s in r.card.segments] == ["result"])
        assert not codes(result)
        assert_reference(result)
