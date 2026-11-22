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
        "_outputs": outputs, "_nix": "{ pkgs, inputs, params }: " + body,
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

    assert ("multiple_input_wires" in codes(resolved["sink"])) == (destination == "left")
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
    assert codes(sink) == {"multiple_input_wires"}
    assert "Merge Card" in text(sink.diagnostics[0].message)
    assert text(sink.inputs[0].rootType) == "t.Number"
    assert sink.inputs[1].rootType is None


def test_merge_reports_distinct_member_names_on_inherited_ports(core_factory, project):
    _, resolved = inspect_graph(core_factory, project, {
        "one": code(), "two": code(outputs=FLAG),
        "merge": {"type": "merge", "inputs": [{"name": n} for n in ("a", "b", "c", "unused")]},
        "fork": {"type": "fork"}, "sink": code(["in"]),
    }, [conn("one", "out", "merge", "a"), conn("one", "out", "merge", "b"),
        conn("two", "flag", "merge", "c"), conn("merge", "out", "fork", "value"),
        conn("fork", "a", "sink", "in")])

    for port in [resolved["merge"].outputs[0], resolved["fork"].inputs[0], resolved["sink"].inputs[0]]:
        assert [text(name) for name in port.mergeMemberTypes] == ["Number", "Flag"]


@pytest.mark.parametrize("schema", [
    "namespace t; table Value { value:int; }",
    'attribute "tsugumi"; table File { path:string; } table Outputs { file:File (tsugumi:"path"); } root_type Outputs;',
], ids=["missing-output-catalog", "path-with-nonstring-field"])
def test_invalid_output_types_are_reported_to_the_client(core_factory, project, schema):
    """A Card with an invalid output catalog exposes a diagnostic to its client."""
    _, resolved = inspect_graph(core_factory, project, {"source": code(outputs=schema)}, [])
    assert "invalid_outputs" in codes(resolved["source"])


def test_port_types_flow_through_branch(core_factory, project):
    """A downstream Card sees the original value type through a Branch."""
    _, resolved = inspect_graph(core_factory, project, {
        "source": code(), "branch": {"type": "branch"},
        "condition": {"type": "input", "params": [{"name": "value", "type": "bool", "data": True}], "_outputs": FLAG},
        "sink": code(["in"]),
    }, [conn("source", "out", "branch", "value"),
        conn("condition", "flag", "branch", "condition"), conn("branch", "a", "sink", "in")])

    incoming = resolved["sink"].inputs[0]
    assert text(incoming.rootType) == "t.Number"
    assert len(incoming.schemaBfbs) > 0
    assert not codes(resolved["sink"])


def assert_reference(node, name, target):
    ref, = node.nixReferences
    assert text(ref.name) == name
    assert [text(segment) for segment in ref.target.segments] == target


def test_fork_outputs_carry_the_value_type(core_factory, project):
    """A Fork has outputs a and b by default, or the declared ones, all typed as its value."""
    _, resolved = inspect_graph(core_factory, project, {
        "source": code(), "pair": {"type": "fork"},
        "three": {"type": "fork", "outputs": [{"name": "x"}, {"name": "y"}, {"name": "z"}]},
        "broken": {"type": "fork", "outputs": [{"name": "x"}, {"name": "x"}, {"name": "not valid"}]},
        "sink": code(["in"]),
    }, [conn("source", "out", "pair", "value"), conn("source", "out", "three", "value"),
        conn("source", "out", "broken", "value"), conn("three", "z", "sink", "in")])
    assert [text(p.name) for p in resolved["pair"].outputs] == ["a", "b"]
    assert [text(p.name) for p in resolved["three"].outputs] == ["x", "y", "z"]
    assert {text(p.rootType) for p in resolved["three"].outputs} == {"t.Number"}
    assert not codes(resolved["three"]) and not codes(resolved["sink"])
    assert [text(p.name) for p in resolved["broken"].outputs] == ["x"]
    assert codes(resolved["broken"]) == {"invalid_setting"}


@pytest.mark.parametrize("builtin", ["branch", "fork", "merge", "collect"])
def test_nix_exports_follow_the_connected_code_origin(core_factory, project, builtin):
    """A transparent built-in keeps the Code producer's exports on the destination port."""
    definitions = {"source": code(), "sink": code(["data"], body='{ run = inputs.data.command; }'),
                   "bridge": {"type": builtin}}
    if builtin == "branch":
        definitions["condition"] = {"type": "input", "_outputs": FLAG}
        connections = [conn("source", "out", "bridge", "value"), conn("condition", "flag", "bridge", "condition"),
                       conn("bridge", "a", "sink", "data")]
    elif builtin == "fork":
        connections = [conn("source", "out", "bridge", "value"), conn("bridge", "b", "sink", "data")]
    elif builtin == "merge":
        definitions["bridge"]["inputs"] = [{"name": "a"}, {"name": "b"}]
        connections = [conn("source", "out", "bridge", "a"), conn("source", "out", "bridge", "b"), conn("bridge", "out", "sink", "data")]
    else:
        connections = [conn("source", "out", "bridge", "in"), conn("bridge", "out", "sink", "data")]
    _, resolved = inspect_graph(core_factory, project, definitions, connections)
    assert not codes(resolved["sink"])
    assert_reference(resolved["sink"], "data", ["source"])


def test_nix_export_producer_must_run_before_every_merge_arrival(core_factory, project):
    _, resolved = inspect_graph(core_factory, project, {
        "first": code(), "second": code(),
        "join": {"type": "merge", "inputs": [{"name": "left"}, {"name": "right"}]},
        "sink": code(["data"], body='{ run = inputs.data.command; }'),
    }, [conn("first", "out", "join", "left"), conn("second", "out", "join", "right"),
        conn("join", "out", "sink", "data")])
    assert not codes(resolved["sink"])
    assert not resolved["sink"].nixReferences


@pytest.mark.parametrize("name", ["tsugumi", "sources"])
def test_builtin_nix_input_names_are_reserved(core_factory, project, name):
    """A data wire cannot replace the SDK or pinned-source bindings in Nix."""
    _, resolved = inspect_graph(core_factory, project, {"source": code(), "sink": code([name])},
                                [conn("source", "out", "sink", name)])
    assert codes(resolved["sink"]) == {"nix_input_reserved"}


def test_nested_cards_keep_their_types_references_and_run_boundaries(core_factory, project):
    """Nix exports and typed values follow the flattened Workflow boundary."""
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
    assert_reference(resolved["sink"], "in", ["child", "inner"])
    assert_reference(resolved["child/inner"], "in", ["source"])
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
                                    {"source": code(), "result": code(["data"], body='{ run = inputs.data.command; }')},
                                    [conn("source", "out", "result", "data")])
    assert_reference(resolved["result"], "data", ["source"])
    for body in [
        '{ run = inputs.data.command; exports.again = inputs.data; }',
        '{ run = inputs.ghost.command; }',
        '{ /* inputs.data */ run = "true"; exports.note = "inputs.ghost"; }',
    ]:
        snap = client.call("ProjectInspectRequest")
        client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="test", changes=[
            change("PutSource", path="definitions/result/default.nix",
                   baseHash=next(text(s.hash) for s in snap.sources if text(s.path) == "definitions/result/default.nix"),
                   bytes=list(('{ pkgs, inputs, params }: ' + body).encode()))])
        snap = client.call("ProjectInspectRequest")
        result = next(r for r in snap.resolution if [text(s) for s in r.card.segments] == ["result"])
        assert not codes(result)
        assert_reference(result, "data", ["source"])
