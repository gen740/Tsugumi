"""Draft plans, wires and human names saved through the real core RPC."""

import json

from tsugumi_client import change, new_id, text


def test_wired_drafts_can_be_saved_written_and_renamed(client, daemon):
    """Saving a wired Draft, editing its plan and renaming it keeps the core and exact addresses alive."""
    before = client.call("ProjectInspectRequest")
    project = json.loads(text(before.projectJson))
    project["canvas"] = {
        "cards": [{"id": id_} for id_ in ("p", "q", "r")],
        "connections": [
            {"from": {"card": "p", "port": ""}, "to": {"card": "q", "port": ""}},
            {"from": {"card": "p", "port": ""}, "to": {"card": "r", "port": ""}},
            {"from": {"card": "r", "port": ""}, "to": {"card": "q", "port": ""}},
        ],
    }
    cards = {
        "p": {"type": "draft"},
        "q": {"type": "draft"},
        "r": {"type": "draft"},
    }
    meta = {
        "p": {"name": "p", "description": "", "content": ""},
        "q": {"name": "q", "description": "", "content": ""},
        "r": {"name": "r", "description": "", "content": ""},
    }
    layout = '{ "cards": {"p":{"x":0,"y":0},"q":{"x":300,"y":0},"r":{"x":600,"y":0}} }'
    changes = [change("SetLayout", json=layout), change("SetProject", json=json.dumps(project))]
    changes += [change("PutDefinition", id=id_, cardJson=json.dumps(card), metaJson=json.dumps(meta[id_]))
                for id_, card in cards.items()]
    op = new_id()
    saved = client.call("ProjectSaveRequest", baseRev=before.rev, opId=op, origin="ui:test", changes=changes)
    assert saved.rev == before.rev + 1
    assert text(saved.delta.layoutJson) == layout
    resolved = {text(card.card.segments[0]): card for card in saved.delta.resolution}
    assert set(resolved) == {"p", "q", "r"}
    assert all(not card.inputs and not card.outputs for card in resolved.values())
    assert json.loads(text(saved.delta.projectJson)) == project
    assert not any(text(d.code) == "unknown_port" for card in resolved.values() for d in (card.diagnostics or []))
    # Replaying copies the same saved resolution into a fresh RPC response.
    replayed = client.call("ProjectSaveRequest", baseRev=before.rev, opId=op, origin="ui:test", changes=changes)
    assert replayed.replayed and replayed.rev == saved.rev
    assert json.loads(text(replayed.delta.projectJson)) == project
    assert {text(card.card.segments[0]) for card in replayed.delta.resolution} == {"p", "q", "r"}
    assert all(not card.inputs and not card.outputs for card in replayed.delta.resolution)

    # Match the canvas: remove two wires, then write the plan. Drafts stay portless.
    project["canvas"]["connections"] = project["canvas"]["connections"][1:2]
    saved = client.call("ProjectSaveRequest", baseRev=saved.rev, opId=new_id(), origin="ui:test", changes=[
        change("SetProject", json=json.dumps(project)),
        *[change("PutDefinition", id=id_, cardJson=json.dumps(card), metaJson=json.dumps(meta[id_]))
          for id_, card in cards.items()],
    ])
    meta["p"].update(name="renamed", content="count words")
    meta["q"]["name"] = "日本語"
    saved = client.call("ProjectSaveRequest", baseRev=saved.rev, opId=new_id(), origin="ui:test", changes=[
        change("PutDefinition", id=id_, cardJson=json.dumps(cards[id_]), metaJson=json.dumps(meta[id_]))
        for id_ in ("p", "q")
    ])
    assert daemon.proc.poll() is None, daemon.diagnostics()
    # Another connection sees the saved plan, name and unchanged layout/addresses.
    after = daemon.connect().call("ProjectInspectRequest")
    assert after.rev == saved.rev
    assert text(after.layoutJson) == layout
    assert json.loads(text(after.projectJson)) == project
    definitions = {text(d.id): d for d in after.definitions}
    for id_ in cards:
        assert json.loads(text(definitions[id_].cardJson)) == cards[id_]
        assert json.loads(text(definitions[id_].metaJson)) == meta[id_]
    # The core speaks in ids: the display name stays in the Card's own _meta.json.
    assert {text(card.card.segments[0]) for card in after.resolution} == {"p", "q", "r"}
    assert all(not card.inputs and not card.outputs for card in after.resolution)
