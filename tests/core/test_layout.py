"""Layout proposals go through the project RPC without mutating the document."""

import json
from pathlib import Path

import pytest

from tsugumi_client import RpcFailure, make, text


def scene_object(scene):
    fields = {key: scene[key] for key in ("flow", "grid", "snap")}
    for field, table in [("cards", "LayoutCard"), ("wires", "LayoutWire"),
                         ("frames", "LayoutBox"), ("notes", "LayoutBox")]:
        fields[field] = []
        for item in scene[field]:
            item = dict(item)
            if field == "cards":
                for side in ("inputs", "outputs"):
                    item[side] = [make("LayoutPort", **port) for port in item[side]]
            fields[field].append(make(table, **item))
    for side in ("inputs", "outputs"):
        if side not in scene:
            continue
        bar = scene[side]
        fields[side] = make("LayoutBar", ends=[make("LayoutBarEnd", **e) for e in bar["ends"]],
                            **bar.get("at", {}))
    return make("LayoutScene", **fields)


def proposal_object(proposal):
    out = {key: [{"id": text(p.id), "x": p.x, "y": p.y} for p in getattr(proposal, key)]
           for key in ("cards", "frames", "notes")}
    out["portOrders"] = [{"id": text(o.id), "inputs": [text(p) for p in o.inputs],
                          "outputs": [text(p) for p in o.outputs]} for o in proposal.portOrders]
    return out


def test_layout_rpc_matches_golden_without_saving(client):
    """Both layout requests return the TypeScript goldens and leave the revision and layout untouched."""
    before = client.call("ProjectInspectRequest")
    vectors = json.loads((Path(__file__).parents[1] / "fixtures/layout-golden.json").read_text())
    for case in vectors:
        fields = {"scene": scene_object(case["scene"])}
        if case["request"] == "untangle":
            fields["around"] = case.get("around", [])
        proposal = client.call("LayoutArrangeRequest" if case["request"] == "arrange" else "LayoutUntangleRequest", **fields)
        assert proposal_object(proposal) == case["expect"], case["name"]
    after = client.call("ProjectInspectRequest")
    assert after.rev == before.rev
    assert after.layoutJson == before.layoutJson
    assert after.projectJson == before.projectJson


@pytest.mark.parametrize("field,value", [("grid", -1), ("grid", float("nan")),
                                        ("grid", float("inf")), ("flow", "unknown")])
def test_layout_rejects_invalid_measurements(client, field, value):
    """Invalid geometry is rejected before arithmetic can produce a proposal."""
    scene = {"flow": "topToBottom", "grid": 20, "snap": False,
             "cards": [], "wires": [], "frames": [], "notes": [], field: value}
    with pytest.raises(RpcFailure) as failure:
        client.call("LayoutArrangeRequest", scene=scene_object(scene))
    assert failure.value.kind == "invalid_request"
