"""Layout proposals go through the host RPC without mutating the document."""

import pytest

import importlib
from tsugumi_client import text
from harness.host import Host, HostFailure


def make(name, /, **fields):
    value = getattr(importlib.import_module(f"Tsugumi.Host.{name}"), name + "T")()
    for key, item in fields.items():
        setattr(value, key, item)
    return value


@pytest.fixture
def host(tmp_path):
    value = Host(tmp_path / "host")
    yield value
    value.close()


def scene_object(scene):
    fields = {key: scene[key] for key in ("flow", "grid", "snap")}
    for field, table in [("cards", "LayoutCard"), ("wires", "LayoutWire"),
                         ("frames", "LayoutBox"), ("notes", "LayoutBox")]:
        fields[field] = []
        for item in scene[field]:
            # Fields named like Python keywords carry a trailing underscore in the generated classes.
            item = {("from_" if key == "from" else key): value for key, value in item.items()}
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


@pytest.mark.parametrize("field,value", [("grid", -1), ("grid", float("nan")),
                                        ("grid", float("inf")), ("flow", "unknown")])
def test_layout_rejects_invalid_measurements(host, field, value):
    """Invalid geometry is rejected before arithmetic can produce a proposal."""
    scene = {"flow": "topToBottom", "grid": 20, "snap": False,
             "cards": [], "wires": [], "frames": [], "notes": [], field: value}
    with pytest.raises(HostFailure) as failure:
        host.call("LayoutArrangeRequest", scene=scene_object(scene))
    assert failure.value.code == "invalid_request"
    # Refusal must leave the same connection usable.
    assert text(host.call("LayoutValidateRequest", json=" { } ").json) == " { } "
