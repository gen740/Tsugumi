"""Document schemas exercised by saving and inspecting public RPC documents."""

import json

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


@pytest.mark.parametrize("key,value", [
    ("boundar", "{}"), ("boundaryX", "{}"), ("boundariesX", "{}"), ("Boundary", "{}"),
    ("boundarie", "{}"), (r"boundary\u0000", "{}"), ("", "{}"),
    ("flo", '"leftToRight"'), ("flows", '"leftToRight"'), ("Flow", '"leftToRight"'),
    (" flow", '"leftToRight"'), ("flow ", '"leftToRight"'), ("flöw", '"leftToRight"'),
    ("フロー", '"leftToRight"'), (r"fl\u006fw", '"leftToRight"'),
], ids=["truncated-boundary", "extended-boundary", "extended-boundaries", "uppercase-boundary",
        "near-boundaries", "nul-suffix", "empty-key", "truncated-flow", "plural-flow", "uppercase-flow",
        "leading-space", "trailing-space", "accented-flow", "translated-flow", "escaped-flow"])
def test_layout_rejects_near_miss_field_names(client, key, value):
    """A field resembling a layout setting is rejected instead of silently misread."""
    before = client.call("ProjectInspectRequest")
    raw = '{"' + key + '":' + value + '}'
    with pytest.raises(RpcFailure) as failure:
        client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                    changes=[change("SetLayout", json=raw)])
    assert failure.value.kind == "invalid_document"
    assert client.call("ProjectInspectRequest").rev == before.rev


def test_layout_rejects_coordinates_that_overflow_a_number(client):
    """A layout coordinate must be finite even when its JSON number is valid."""
    before = client.call("ProjectInspectRequest")
    with pytest.raises(RpcFailure) as failure:
        client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                    changes=[change("SetLayout", json='{"boundary":{"inputs":{"x":1e999,"y":0}}}')])
    assert failure.value.kind == "invalid_document"
    assert client.call("ProjectInspectRequest").rev == before.rev
