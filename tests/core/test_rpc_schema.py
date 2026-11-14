"""Document schemas exercised by saving and inspecting public RPC documents."""

import json
from pathlib import Path

import pytest

from tsugumi_client import RpcFailure, change, new_id, text


@pytest.mark.parametrize("raw", [
    '{"sandbox":true,"sandbox":false}',
    '{"params":[{"name":"a","data":{"k":1,"k":2}}]}',
    b'{"symbol":"\xff"}',
    b'{"symbol":"\xc3"}',
    '{"timeoutSeconds":NaN}',
    '{"timeoutSeconds":Infinity}',
    '{"sandbox":"true"}',
    '{"maxRuns":-1}',
    '{"maxRuns":1.5}',
    '{"maxRuns":18446744073709551616}',
    '{"sandbox":true} trailing',
    '{"sandbox":null}',
], ids=["duplicate-field", "nested-duplicate-field", "invalid-utf8", "truncated-utf8",
        "not-a-number", "infinity", "boolean-as-string", "negative-run-limit",
        "fractional-run-limit", "overflowing-run-limit", "trailing-text", "null-boolean"])
def test_invalid_card_documents_are_rejected_without_a_save(client, raw):
    """Invalid card fields are rejected without changing the saved project."""
    before = client.call("ProjectInspectRequest")
    with pytest.raises(RpcFailure) as failure:
        client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                    changes=[change("PutDefinition", id="work", cardJson=raw)])
    assert failure.value.kind == "invalid_document"
    after = client.call("ProjectInspectRequest")
    assert after.rev == before.rev
    assert all(text(definition.id) != "work" for definition in after.definitions)


def test_valid_utf8_null_timeout_and_integer_spellings_survive_a_save(client):
    """Valid Unicode, an unset timeout and exact integer spellings survive a save."""
    raw = ('{"template":"draft","symbol":"é","timeoutSeconds":null,'
           '"maxRuns":18446744073709551615,'
           '"params":[{"name":"decimal","data":1.50e+2},'
           '{"name":"minimum","data":-9223372036854775808}]}')
    before = client.call("ProjectInspectRequest")
    client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                changes=[change("PutDefinition", id="work", cardJson=raw)])
    after = client.call("ProjectInspectRequest")
    saved = next(definition for definition in after.definitions if text(definition.id) == "work")
    assert text(saved.cardJson) == raw
    assert json.loads(text(saved.cardJson))["timeoutSeconds"] is None


@pytest.mark.parametrize("order", ["boundary-first", "boundaries-first"])
def test_layout_fields_with_a_shared_prefix_remain_distinct(client, order):
    """Boundary positions and nested boundary positions keep their own values."""
    fields = [
        ("boundary", {"outputs": {"x": 3, "y": 4}}),
        ("boundaries", {"child": {"inputs": {"x": 1, "y": 2}}}),
    ]
    if order == "boundaries-first":
        fields.reverse()
    layout = dict(fields)
    before = client.call("ProjectInspectRequest")
    saved = client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                        changes=[change("SetLayout", json=json.dumps(layout))])
    assert json.loads(text(saved.delta.layoutJson)) == layout
    assert json.loads(text(client.call("ProjectInspectRequest").layoutJson)) == layout


@pytest.mark.parametrize("case", json.loads(
    (Path(__file__).parents[1] / "fixtures/layout-invalid.json").read_text()),
    ids=lambda case: case["name"])
def test_core_stores_opaque_layout_unchanged(client, case):
    """Layout validation belongs to the host; the core preserves even unknown or unbounded fields."""
    before = client.call("ProjectInspectRequest")
    raw = case["json"]
    saved = client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                        changes=[change("SetLayout", json=raw)])
    assert text(saved.delta.layoutJson) == raw
    after = client.call("ProjectInspectRequest")
    assert after.rev == before.rev + 1
    assert text(after.layoutJson) == raw
