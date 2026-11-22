"""Document schemas exercised by saving and inspecting public RPC documents."""

import json

import pytest

from tsugumi_client import RpcFailure, change, new_id, text


def test_invalid_card_documents_are_rejected_without_a_save(client):
    """Invalid card fields are rejected without changing the saved project."""
    before = client.call("ProjectInspectRequest")
    invalid_documents = (
        '{"type":"code","sandbox":true,"sandbox":false}',
        '{"type":"code","params":[{"name":"a","data":{"k":1,"k":2}}]}',
        b'{"type":"code","symbol":"\xff"}',
        b'{"type":"code","symbol":"\xc3"}',
        '{"type":"code","timeoutSeconds":NaN}',
        '{"type":"code","timeoutSeconds":Infinity}',
        '{"type":"code","sandbox":"true"}',
        '{"type":"code","maxRuns":-1}',
        '{"type":"code","maxRuns":1.5}',
        '{"type":"code","maxRuns":18446744073709551616}',
        '{"type":"code","sandbox":{}} trailing',
        '{"type":"code","sandbox":null}',
        '{"type":"code","sandbox":false}',
        '{"type":"code","sandbox":{"pid":"false"}}',
        '{"type":"code","grants":{"gui":null}}',
        '{"type":"code","unknownCardField":true}',
        '{"type":"code","inputs":[{"name":"old","optional":true}]}',
        '{"type":"code","trigger":"eachInput"}',
        '{}',
        '{"type":"none"}',
    )
    for raw in invalid_documents:
        with pytest.raises(RpcFailure) as failure:
            client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                        changes=[change("PutDefinition", id="work", cardJson=raw)])
        assert failure.value.kind == "invalid_document", raw
    after = client.call("ProjectInspectRequest")
    assert after.rev == before.rev
    assert all(text(definition.id) != "work" for definition in after.definitions)

    # A valid document on the same connection preserves spelling and nulls.
    raw = ('{"type":"draft","symbol":"é","timeoutSeconds":null,'
           '"maxRuns":18446744073709551615,'
           '"params":[{"name":"decimal","data":1.50e+2},'
           '{"name":"minimum","data":-9223372036854775808}]}')
    client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                changes=[change("PutDefinition", id="work", cardJson=raw)])
    after = client.call("ProjectInspectRequest")
    saved = next(definition for definition in after.definitions if text(definition.id) == "work")
    assert text(saved.cardJson) == raw




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
        "type": "code", "inputs": [{"name": "data"}],
        "params": [{"name": "limit", "type": "double", "data": 2.5}],
        "maxRuns": 3,
        "timeoutSeconds": 120, "buildTimeoutSeconds": 240,
        "recordIO": True, "retainArtifact": True, "artifactOutput": "report",
        "sandbox": {"pid": False, "ipc": True, "identity": False, "filesystem": True,
                    "limits": {"cpu": 0.5, "memoryMiB": None}},
        "grants": {"network": False, "gui": False, "hostPaths": []},
        "reuseWorker": False, "executionService": "local",
    }
    before = client.call("ProjectInspectRequest")
    saved = client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                        changes=[change("PutDefinition", id="work", cardJson=json.dumps(settings))])
    assert saved.rev == before.rev + 1
    after = client.call("ProjectInspectRequest")
    definition = next(item for item in after.definitions if text(item.id) == "work")
    assert json.loads(text(definition.cardJson)) == settings
