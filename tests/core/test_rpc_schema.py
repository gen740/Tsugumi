"""Document schemas exercised by saving and inspecting public RPC documents."""

import json

import pytest

from tsugumi_client import RpcFailure, change, new_id, text
from harness.document import card_document, card_readme, layout_bytes


def test_invalid_card_documents_are_rejected_without_a_save(client):
    """Invalid card fields are rejected without changing the saved project."""
    before = client.call("ProjectInspectRequest")
    invalid_documents = (
        '{"type":"code","sandbox":true,"sandbox":false}',
        '{"type":"code","params":{"k":1}}',
        '{"type":"input","symbol":"slider.horizontal.3"}',
        b'{"type":"code","symbol":"\xff"}',
        b'{"type":"code","symbol":"\xc3"}',
        '{"type":"code","timeoutSeconds":NaN}',
        '{"type":"code","timeoutSeconds":Infinity}',
        '{"type":"code","sandbox":"true"}',
        '{"type":"code","maxRuns":-1}',
        '{"type":"code","maxRuns":1.5}',
        '{"type":"code","maxRuns":18446744073709551616}',
        '{"type":"code","sandbox":{}} trailing',
        '{"type":"code","sandbox":false}',
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

    # A valid document keeps exact integer values and writes canonical text.
    settings = card_document({"type": "code", "symbol": "é", "maxRuns": (1 << 64) - 1})
    project = json.loads(text(before.projectJson))
    project["canvas"]["cards"].append({"id": "work"})
    client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                changes=[change("SetProject", json=json.dumps(project)),
                         change("PutDefinition", id="work", cardJson=json.dumps(settings),
                                readme=card_readme("Work"))])
    after = client.call("ProjectInspectRequest")
    saved = next(definition for definition in after.definitions if text(definition.id) == "work")
    parsed = json.loads(text(saved.cardJson))
    assert parsed["maxRuns"] == (1 << 64) - 1
    assert text(saved.cardJson).endswith("\n")




def test_core_stores_opaque_layout_unchanged(client):
    """The core verifies the layout buffer and preserves the exact bytes."""
    before = client.call("ProjectInspectRequest")
    raw = layout_bytes({"cards": {"work": {"x": 1.25, "y": 2.5}}})
    saved = client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                        changes=[change("SetLayout", layout=raw)])
    assert bytes(saved.delta.layout) == raw
    after = client.call("ProjectInspectRequest")
    assert after.rev == before.rev + 1
    assert bytes(after.layout) == raw


def test_card_settings_round_trip(client):
    """Execution settings survive a save and inspect without a filesystem switch."""
    settings = {
        "type": "code", "inputs": [{"name": "data"}],
        "maxRuns": 3,
        "timeoutSeconds": 120, "buildTimeoutSeconds": 240,
        "recordIO": "on",
        "sandbox": {"pid": False, "ipc": True, "identity": False, "filesystem": True,
                    "limits": {"cpu": 0.5}},
        "grants": {"network": False, "gui": False, "hostPaths": []},
        "reuseWorker": False, "workerCapabilities": [0, 63],
    }
    settings = card_document(settings)
    before = client.call("ProjectInspectRequest")
    project = json.loads(text(before.projectJson))
    project["canvas"]["cards"].append({"id": "work"})
    saved = client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test",
                        changes=[change("SetProject", json=json.dumps(project)),
                                 change("PutDefinition", id="work", cardJson=json.dumps(settings),
                                        readme=card_readme("Work"))])
    assert saved.rev == before.rev + 1
    after = client.call("ProjectInspectRequest")
    definition = next(item for item in after.definitions if text(item.id) == "work")
    assert json.loads(text(definition.cardJson)) == settings
