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
    '{"unknownCardField":true}',
], ids=["duplicate-field", "nested-duplicate-field", "invalid-utf8", "truncated-utf8",
        "not-a-number", "infinity", "boolean-as-string", "negative-run-limit",
        "fractional-run-limit", "overflowing-run-limit", "trailing-text", "null-boolean",
        "unknown-card-field"])
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




def test_core_stores_opaque_layout_unchanged(client):
    """The core carries bounded JSON objects without interpreting presentation fields."""
    before = client.call("ProjectInspectRequest")
    raw = r'{"extension":{"escaped\u006bey":"日本語","x":1e999},"boundary":{},"boundaries":{}}'
    saved = client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                        changes=[change("SetLayout", json=raw)])
    assert text(saved.delta.layoutJson) == raw
    after = client.call("ProjectInspectRequest")
    assert after.rev == before.rev + 1
    assert text(after.layoutJson) == raw


def test_card_settings_round_trip(client):
    """Execution settings survive a save and inspect without a filesystem switch."""
    settings = {
        "language": "bash", "inputs": [{"name": "data", "optional": True}],
        "params": [{"name": "limit", "type": "double", "data": 2.5}],
        "trigger": "eachInput", "maxRuns": 3,
        "timeoutSeconds": 120, "buildTimeoutSeconds": 240,
        "recordIO": True, "retainArtifact": True, "artifactOutput": "report",
        "sandbox": True, "network": False, "reuseWorker": False,
        "executionService": "local", "hostFolders": [],
    }
    before = client.call("ProjectInspectRequest")
    saved = client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                        changes=[change("PutDefinition", id="work", cardJson=json.dumps(settings))])
    assert saved.rev == before.rev + 1
    after = client.call("ProjectInspectRequest")
    definition = next(item for item in after.definitions if text(item.id) == "work")
    assert json.loads(text(definition.cardJson)) == settings
