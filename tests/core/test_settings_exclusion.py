"""Every settings writer shares ProjectSave's revision exclusion."""
import json

import pytest

from tsugumi_client import RpcFailure, change, new_id, text


def edit(snapshot, target, value):
    if target == "definition":
        return [change("PutDefinition", id="settings", cardJson=json.dumps({"template": "draft", "symbol": value}))]
    project = json.loads(text(snapshot.projectJson))
    project["name"] = value
    return [change("SetProject", json=json.dumps(project))]


def saved_value(snapshot, target):
    if target == "definition":
        definition = next(d for d in snapshot.definitions if text(d.id) == "settings")
        return json.loads(text(definition.cardJson))["symbol"]
    return json.loads(text(snapshot.projectJson))["name"]


@pytest.mark.parametrize("target", ["definition", "project"])
@pytest.mark.parametrize("writer,stale_writer", [("agent", "ui:json"), ("ui:card", "agent")])
def test_stale_settings_apply_and_explicit_overwrite(daemon, target, writer, stale_writer):
    """Both directions reject stale text; choosing overwrite still needs an exact base revision."""
    first, second = daemon.connect(), daemon.connect()
    opened = second.call("ProjectInspectRequest")
    stale_text = edit(opened, target, "mine")

    def save(client, snapshot, origin, changes):
        return client.call("ProjectSaveRequest", baseRev=snapshot.rev, opId=new_id(), origin=origin, changes=changes)

    save(first, opened, writer, edit(opened, target, "newer"))
    with pytest.raises(RpcFailure) as failure:
        save(second, opened, stale_writer, stale_text)
    assert failure.value.kind == "revision_conflict"
    reviewed = second.call("ProjectInspectRequest")
    assert saved_value(reviewed, target) == "newer"

    # The other writer's next save wins before the human's overwrite reaches
    # the core (or the Agent's next step uses the base preceding the UI save).
    save(first, reviewed, writer, edit(reviewed, target, "newest"))
    with pytest.raises(RpcFailure) as failure:
        save(second, reviewed, stale_writer, stale_text)
    assert failure.value.kind == "revision_conflict"
    latest = second.call("ProjectInspectRequest")
    assert saved_value(latest, target) == "newest"

    result = save(second, latest, stale_writer, stale_text)
    assert result.rev == latest.rev + 1
    assert saved_value(first.call("ProjectInspectRequest"), target) == "mine"


@pytest.mark.parametrize("path", ["tsugumi.json", "definitions/settings/_card.json", "definitions/settings/_meta.json"])
def test_settings_have_no_source_write_bypass(client, path):
    """Source RPC changes cannot provide a second settings writer."""
    before = client.call("ProjectInspectRequest")
    for operation, fields in [
        ("PutSource", {"bytes": list(b"{}")}),
        ("PatchSource", {"baseHash": "0" * 64, "patches": []}),
        ("RemoveSource", {"baseHash": "0" * 64}),
    ]:
        with pytest.raises(RpcFailure) as failure:
            client.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:json",
                        changes=[change(operation, path=path, **fields)])
        assert failure.value.kind == "permission_denied"
        assert client.call("ProjectInspectRequest").rev == before.rev
