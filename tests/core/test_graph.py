"""Port resolution, Nix references and run plans through public RPC."""

import pytest

from projects import card, card_in_card, conn, write
from tsugumi_client import path, text


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


@pytest.mark.parametrize("shape,reference,expected_error", [
    ("chain", "a", None), ("chain", "b", None),
    ("branch", "a", None), ("branch", "b", "nix_reference_not_guaranteed"),
    ("branch", "c", "nix_reference_not_guaranteed"),
    ("join", "b", None), ("join", "c", None), ("join", "optional", "nix_reference_not_guaranteed"),
    ("same-input", "a", None), ("same-input", "b", "nix_reference_not_guaranteed"),
    ("loop", "a", None), ("loop", "result", "nix_reference_self"),
    ("unreachable-cycle", "other", "nix_reference_not_guaranteed"),
], ids=["chain-first", "chain-last", "branch-common", "branch-left", "branch-right",
        "join-left", "join-right", "optional-input", "same-input-common", "same-input-left",
        "loop-initial", "loop-self", "unreachable-cycle"])
def test_nix_can_reference_only_guaranteed_upstream_cards(core_factory, project, shape, reference, expected_error):
    """A build can use an upstream Card only when every path provides it."""
    definitions = {"a": code(), "b": code(["in"]), "c": code(["in"])}
    connections = [conn("a", "out", "b", "in")]
    result = code(["in"])
    if shape == "chain":
        connections += [conn("b", "out", "result", "in")]
    elif shape in ("join", "same-input"):
        connections += [conn("a", "out", "c", "in")]
        if shape == "join":
            definitions["optional"] = code()
            result = code(["left", "right", "extra?"])
            connections += [conn("b", "out", "result", "left"), conn("c", "out", "result", "right"),
                            conn("optional", "out", "result", "extra")]
        else:
            connections += [conn("b", "out", "result", "in"), conn("c", "out", "result", "in")]
    elif shape == "branch":
        definitions.update(branch={"template": "branch"}, condition={"template": "input", "_outputs": FLAG,
                           "params": [{"name": "value", "type": "bool", "data": True}]})
        result = code(["left?", "right?"], each_input=True)
        connections = [conn("a", "out", "branch", "value"), conn("condition", "flag", "branch", "condition"),
                       conn("branch", "a", "b", "in"), conn("branch", "b", "c", "in"),
                       conn("b", "out", "result", "left"), conn("c", "out", "result", "right")]
    elif shape == "loop":
        definitions = {"a": code(), "loop": code(["initial?", "again?"], each_input=True)}
        connections = [conn("a", "out", "loop", "initial"), conn("loop", "out", "result", "in"),
                       conn("result", "out", "loop", "again")]
    else:
        definitions = {"other": code(["in"])}
        connections = [conn("other", "out", "result", "in"), conn("result", "out", "other", "in")]
    result["_nix"] = '{ pkgs, inputs, params }: { run = "${inputs.upstream.\"' + reference + '\".run}"; }'
    definitions["result"] = result
    _, resolved = inspect_graph(core_factory, project, definitions, connections)

    errors = codes(resolved["result"]) & {"nix_reference_unknown", "nix_reference_self", "nix_reference_not_guaranteed"}
    assert errors == ({expected_error} if expected_error else set())


@pytest.mark.parametrize("expression,expected", [
    ('{ run = "${inputs.local.source.run}"; }', set()),
    ('{ run = "${inputs.local.\"source/deeper\".run}"; }', {"nix_reference_unknown"}),
    ('''{ # upstream.ghost
       /* local.ghost */
       run = ''cp local.conf "$out"; echo ''${upstream.escaped}'';
       note = "upstream.ghost";
     }''', set()),
    ('{ run = "${inputs.upstream.ghost.run}"; }', {"nix_reference_unknown"}),
], ids=["local-sibling", "local-cannot-enter-child", "comments-and-literal-text", "interpolation-is-code"])
def test_only_executable_nix_references_are_validated(core_factory, project, expression, expected):
    """Reference checks distinguish Nix code from comments and shell text."""
    _, resolved = inspect_graph(core_factory, project,
                                {"source": code(), "result": code(["in"], body=expression)},
                                [conn("source", "out", "result", "in")])
    assert codes(resolved["result"]) == expected


def test_nested_cards_keep_their_types_references_and_run_boundaries(core_factory, project):
    """A nested Workflow exposes its values and the run plan respects its boundary."""
    definitions = {"start": {"template": "trigger"}, "source": code(), "inner": code(["in"]),
                   "sink": code(["in"], body='{ run = "${inputs.upstream.\"child/inner\".run}"; }')}
    cards = [card("start", "start"), card("source", "source"),
             card_in_card("child", [card("inner", "inner")], [],
                          {"inputs": {"in": {"card": "inner", "port": "in"}},
                           "outputs": {"out": {"card": "inner", "port": "out"}}}), card("sink", "sink")]
    client, resolved = inspect_graph(core_factory, project, definitions,
                                     [conn("source", "out", "child", "in"), conn("child", "out", "sink", "in")], cards)
    assert not codes(resolved["sink"])
    assert text(resolved["sink"].inputs[0].rootType) == "t.Number"
    for target, propagate, expected in [
        ("start", True, {"start", "source", "child/inner", "sink"}),
        ("sink", False, {"sink"}), ("child", False, {"child/inner"}),
        ("child", True, {"child/inner", "sink"}),
    ]:
        plan = client.call("RunPlanRequest", card=path(target), propagate=propagate)
        assert {"/".join(text(segment) for segment in target.segments) for target in plan.targets} == expected
