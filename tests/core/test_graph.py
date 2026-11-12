"""Port resolution, Nix references and run plans through public RPC."""

import pytest

from projects import card, card_in_card, conn, write
from tsugumi_client import change, new_id, path, text


NUMBER = 'namespace t; table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;'
FLAG = 'namespace t; table Flag { value:bool; } table Outputs { flag:Flag (required); } root_type Outputs;'


def code(inputs=(), *, outputs=NUMBER, body='{ run = "true"; }', each_input=False):
    return {
        "language": "bash", "sandbox": False,
        "trigger": "eachInput" if each_input else "allInputs",
        "inputs": [{"name": name.rstrip("?"), "optional": name.endswith("?")} for name in inputs],
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


@pytest.mark.parametrize("fields,compatible", [
    ("x:int; s:string;", True),
    ("x:long; s:string;", False),
    ("s:string; x:int;", False),
], ids=["different-namespace-same-fields", "different-field-type", "different-field-order"])
def test_one_input_accepts_only_structurally_compatible_values(core_factory, project, fields, compatible):
    """Two sources can share an input only when their value structures agree."""
    def schema(namespace, members):
        return f"namespace {namespace}; table Value {{ {members} }} table Outputs {{ out:Value; }} root_type Outputs;"

    _, resolved = inspect_graph(core_factory, project, {
        "one": code(outputs=schema("one", "x:int; s:string;")),
        "two": code(outputs=schema("two", fields)), "sink": code(["in"]),
    }, [conn("one", "out", "sink", "in"), conn("two", "out", "sink", "in")])

    assert ("type_mismatch" not in codes(resolved["sink"])) == compatible


@pytest.mark.parametrize("schema", [
    "namespace t; table Value { value:int; }",
    'attribute "tsugumi"; table File { path:string; } table Outputs { file:File (tsugumi:"file"); } root_type Outputs;',
], ids=["missing-output-catalog", "file-without-string-value"])
def test_invalid_output_types_are_reported_to_the_client(core_factory, project, schema):
    """A Card with an invalid output catalog exposes a diagnostic to its client."""
    _, resolved = inspect_graph(core_factory, project, {"source": code(outputs=schema)}, [])
    assert "invalid_outputs" in codes(resolved["source"])


def test_port_types_flow_through_fork_and_branch(core_factory, project):
    """A downstream Card sees the original value type through forks and branches."""
    _, resolved = inspect_graph(core_factory, project, {
        "source": code(), "fork": {"template": "fork"}, "branch": {"template": "branch"},
        "condition": {"template": "input", "params": [{"name": "value", "type": "bool", "data": True}], "_outputs": FLAG},
        "sink": code(["in"]),
    }, [conn("source", "out", "fork", "in"), conn("fork", "out1", "branch", "value"),
        conn("condition", "flag", "branch", "condition"), conn("branch", "a", "sink", "in")])

    incoming = resolved["sink"].inputs[0]
    assert text(incoming.rootType) == "t.Number"
    assert len(incoming.schemaBfbs) > 0
    assert not codes(resolved["sink"])


def assert_reference(node, name, target, valid=True):
    ref, = node.nixReferences
    assert text(ref.name) == name
    assert [text(segment) for segment in ref.target.segments] == target
    assert ref.valid == valid


@pytest.mark.parametrize("builtin", ["fork", "branch", "merge", "collect"])
def test_nix_exports_follow_the_connected_code_origin(core_factory, project, builtin):
    """A transparent built-in keeps the Code producer's exports on the destination port."""
    definitions = {"source": code(), "sink": code(["data"], body='{ run = inputs.data.command; }'),
                   "bridge": {"template": builtin}}
    if builtin == "branch":
        definitions["condition"] = {"template": "switch"}
        connections = [conn("source", "out", "bridge", "value"), conn("condition", "value", "bridge", "condition"),
                       conn("bridge", "a", "sink", "data")]
    elif builtin == "merge":
        definitions["fork"] = {"template": "fork"}
        definitions["bridge"]["inputs"] = [{"name": "a"}, {"name": "b"}]
        connections = [conn("source", "out", "fork", "in"), conn("fork", "out1", "bridge", "a"),
                       conn("fork", "out2", "bridge", "b"), conn("bridge", "out", "sink", "data")]
    else:
        output = "out1" if builtin == "fork" else "out"
        connections = [conn("source", "out", "bridge", "in"), conn("bridge", output, "sink", "data")]
    _, resolved = inspect_graph(core_factory, project, definitions, connections)
    assert not codes(resolved["sink"])
    assert_reference(resolved["sink"], "data", ["source"])


@pytest.mark.parametrize("reference", [False, True], ids=["ordinary-data", "nix-export-reference"])
def test_ambiguous_producer_is_rejected_only_for_nix_exports(core_factory, project, reference):
    """Multiple compatible data producers remain valid until a Card asks for their exports."""
    body = '{ run = inputs.data.command; }' if reference else '{ run = "true"; }'
    _, resolved = inspect_graph(core_factory, project,
                                {"first": code(), "second": code(), "sink": code(["data"], body=body)},
                                [conn("first", "out", "sink", "data"), conn("second", "out", "sink", "data")])
    diagnostics = codes(resolved["sink"])
    if reference:
        assert diagnostics == {"nix_reference_ambiguous"}
        assert resolved["sink"].nixReferences and not any(ref.valid for ref in resolved["sink"].nixReferences)
    else:
        assert not diagnostics
        assert not resolved["sink"].nixReferences


def test_nix_export_producer_must_run_before_every_trigger(core_factory, project):
    """An each-input Card can start from either input, so neither producer is guaranteed."""
    _, resolved = inspect_graph(core_factory, project, {
        "first": code(), "second": code(),
        "sink": code(["left?", "right?"], each_input=True, body='{ run = inputs.left.command; }'),
    }, [conn("first", "out", "sink", "left"), conn("second", "out", "sink", "right")])
    assert codes(resolved["sink"]) == {"nix_reference_not_guaranteed"}
    assert_reference(resolved["sink"], "left", ["first"], valid=False)


@pytest.mark.parametrize("expression,valid", [
    ('{ run = "${inputs.data.command}"; }', True),
    ('{ run = "${inputs.ghost.command}"; }', False),
    ('{ run = inputs.local.source.command; }', False),
    ('{ run = inputs.upstream.source.command; }', False),
    ('{ /* inputs.ghost.command */ run = "true"; exports.text = "inputs.ghost"; }', None),
], ids=["interpolation", "unknown-port", "removed-local", "removed-upstream", "comments-and-text"])
def test_only_executable_connected_nix_references_are_validated(core_factory, project, expression, valid):
    """Reference checks distinguish executable Nix code from comments and literal text."""
    _, resolved = inspect_graph(core_factory, project,
                                {"source": code(), "result": code(["data"], body=expression)},
                                [conn("source", "out", "result", "data")])
    node = resolved["result"]
    if valid is True:
        assert not codes(node)
        assert_reference(node, "data", ["source"])
    elif valid is False:
        assert codes(node) == {"nix_reference_unknown"}
    else:
        assert not codes(node)
        assert not node.nixReferences


@pytest.mark.parametrize("name", ["tsugumi", "sources"])
def test_builtin_nix_input_names_are_reserved(core_factory, project, name):
    """A data wire cannot replace the SDK or pinned-source bindings in Nix."""
    _, resolved = inspect_graph(core_factory, project, {"source": code(), "sink": code([name])},
                                [conn("source", "out", "sink", name)])
    assert codes(resolved["sink"]) == {"nix_input_reserved"}


def test_nested_cards_keep_their_types_references_and_run_boundaries(core_factory, project):
    """Nix exports and typed values follow the flattened Workflow boundary."""
    definitions = {"start": {"template": "trigger"}, "source": code(),
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


def test_nix_reference_inventory_refreshes_and_deduplicates(core_factory, project):
    client, resolved = inspect_graph(core_factory, project,
                                    {"source": code(), "result": code(["data"], body='{ run = inputs.data.command; }')},
                                    [conn("source", "out", "result", "data")])
    assert_reference(resolved["result"], "data", ["source"])
    for body, expected in [
        ('{ run = inputs.data.command; exports.again = inputs.data; }', [("data", True)]),
        ('{ run = inputs.ghost.command; }', [("ghost", False)]),
        ('{ /* inputs.data */ run = "true"; exports.note = "inputs.ghost"; }', []),
    ]:
        snap = client.call("ProjectInspectRequest")
        client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="test", changes=[
            change("PutSource", path="definitions/result/default.nix",
                   baseHash=next(text(s.hash) for s in snap.sources if text(s.path) == "definitions/result/default.nix"),
                   bytes=list(('{ pkgs, inputs, params }: ' + body).encode()))])
        snap = client.call("ProjectInspectRequest")
        result = next(r for r in snap.resolution if [text(s) for s in r.card.segments] == ["result"])
        assert [(text(r.name), r.valid) for r in result.nixReferences] == expected
