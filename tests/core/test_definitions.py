"""Definition identity, nested projects and layout are observed through public RPC."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from projects import MSG_FBS, builtin_project, card, conn, put, run_and_wait
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState


def project_json(c) -> tuple[int, dict]:
    snap = c.call("ProjectInspectRequest")
    return snap.rev, json.loads(text(snap.projectJson))


def diagnostics(c, card: list[str]) -> list[str]:
    snap = c.call("ProjectInspectRequest")
    for r in snap.resolution:
        if [text(s) for s in r.card.segments] == card:
            return [text(d.code) for d in r.diagnostics]
    raise KeyError(card)


def external(root: Path, name: str, text_value: str) -> None:
    """externals/<name>: input -> logger, its boundary output is the message."""
    ext = root / "externals" / name
    put(ext / "tsugumi.json", json.dumps({
        "name": name, "pkgsPath": "tsugumi:default", "databaseConnections": [],
        "canvas": {"cards": [{"id": "say"}, {"id": "pass"}], "connections": [conn("say", "msg", "pass", "in")]},
        "boundary": {"inputs": {}, "outputs": {"said": {"card": "pass", "port": "out1"}}}}))
    put(ext / "layout.json", "{}")
    put(ext / "definitions" / "say" / "_card.json", json.dumps(
        {"template": "input", "params": [{"name": "text", "type": "string", "data": text_value},
                                         {"name": "value", "type": "bool", "data": True}]}))
    put(ext / "definitions" / "say" / "_outputs.fbs", MSG_FBS)
    put(ext / "definitions" / "pass" / "_card.json", json.dumps({"template": "fork", "params": [
        {"name": "branches", "type": "string[]", "data": ["out1"]}]}))


def test_rename_moves_the_definition_and_its_history(tmp_path, core_factory):
    """Renaming a card preserves its sources and makes its history available under the new name."""
    root = tmp_path / "p"
    builtin_project(root, text="hi")
    put(root / "definitions" / "src" / "notes.txt", "kept")
    d = core_factory(project_root=root)
    c = d.connect()
    run_id, st = run_and_wait(c, "src")
    assert st.state == RunState.succeeded
    rev, doc = project_json(c)
    for card in doc["canvas"]["cards"]:
        if card["id"] == "src":
            card["id"] = "source"
    for e in doc["canvas"]["connections"]:
        for side in ("from", "to"):
            if e[side]["card"] == "src":
                e[side]["card"] = "source"
    with pytest.raises(RpcFailure) as err:
        c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t",
               changes=[change("SetProject", json=json.dumps(doc)), change("RenameDefinition", id="src", to="out")])
    assert err.value.kind == "invalid_request"  # the name is taken
    res = c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t",
                 changes=[change("SetProject", json=json.dumps(doc)), change("RenameDefinition", id="src", to="source")])
    assert "src" in [text(x) for x in res.delta.definitionsRemoved]
    assert bytes(c.call("ProjectFileReadRequest", path="definitions/source/notes.txt").bytes) == b"kept"
    with pytest.raises(RpcFailure) as missing:
        c.call("ProjectFileReadRequest", path="definitions/src/notes.txt")
    assert missing.value.kind == "not_found"
    # The run's records follow the new name.
    rec = c.call("RunRecordRequest", runId=run_id, card=path("source"))
    assert text(rec.record.summary.card.segments[0]) == "source"
    assert c.call("RunInvocationsRequest", runId=run_id, card=path("source")).records
    run2, st2 = run_and_wait(c, "source")
    assert st2.state == RunState.succeeded


def pretty(doc: dict) -> str:
    """layout.json as the clients write it."""
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def spelled(layout: dict) -> str:
    """A number's spelling, which a rewrite of the file keeps."""
    return pretty(layout).replace('"x": 10.5,', '"x": 10.50,')


LAID_OUT = {
    "flow": "topToBottom",
    "cards": {"start": {"x": 0, "y": 0}, "src": {"x": 10.5, "y": 20, "collapsed": True}, "out": {"x": 30, "y": 40},
              "src/inner": {"x": 1, "y": 2}, "src2": {"x": 5, "y": 6}},
    "notes": [{"id": "n", "scope": "", "text": "メモ \"src\"\n改行", "x": 1, "y": 2}],
    "displays": {"src": {"x": 100, "y": 200, "width": 300, "height": 150, "hidden": False},
                 "src2": {"x": 7, "y": 8, "width": 9, "height": 10}},
    "groups": [{"id": "g", "scope": "", "name": "src", "color": "blue", "cards": ["src", "out"],
                "outputs": [{"card": "src", "port": "msg"}, {"card": "out", "port": "x"}]}],
}


def laid_out_project(root: Path, layout: dict = LAID_OUT) -> None:
    builtin_project(root, extra_cards=[card("src2", "log")])
    (root / "layout.json").write_text(spelled(layout), encoding="utf-8")


def renamed_project(doc: dict, old: str, new: str) -> dict:
    for c in doc["canvas"]["cards"]:
        if c["id"] == old:
            c["id"] = new
    for e in doc["canvas"]["connections"]:
        for side in ("from", "to"):
            if e[side]["card"] == old:
                e[side]["card"] = new
    return doc


def without_card(doc: dict, id: str) -> dict:
    doc["canvas"]["cards"] = [c for c in doc["canvas"]["cards"] if c["id"] != id]
    doc["canvas"]["connections"] = [e for e in doc["canvas"]["connections"] if id not in (e["from"]["card"], e["to"]["card"])]
    return doc


def test_an_agent_rename_keeps_the_cards_place_frame_and_group(tmp_path, core_factory):
    """Renaming a card preserves its position, display frame and group membership.

    Unrelated card names and the original layout formatting remain unchanged,
    and the same layout is visible after restarting the daemon.
    """
    root = tmp_path / "p"
    laid_out_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    rev, doc = project_json(c)
    res = c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetProject", json=json.dumps(renamed_project(doc, "src", "source"))),
        change("RenameDefinition", id="src", to="source")])
    moved = json.loads(json.dumps(LAID_OUT))
    moved["cards"] = {{"src": "source", "src/inner": "source/inner"}.get(k, k): v for k, v in moved["cards"].items()}
    moved["displays"] = {{"src": "source"}.get(k, k): v for k, v in moved["displays"].items()}
    moved["groups"][0]["cards"] = ["source", "out"]
    moved["groups"][0]["outputs"][0]["card"] = "source"
    expected = spelled(moved)
    assert text(c.call("ProjectInspectRequest").layoutJson) == expected
    assert text(res.delta.layoutJson) == expected  # the clients learn it from the save
    # It is what the next start reads too.
    d.restart()
    c = d.connect()
    assert text(c.call("ProjectInspectRequest").layoutJson) == expected


def test_a_rename_with_the_clients_own_layout_is_not_moved_twice(tmp_path, core_factory):
    """A rename accepts the client's updated layout without moving it twice.

    Any remaining old-name entry follows the card and replaces a stale entry
    under the new name.
    """
    root = tmp_path / "p"
    laid_out_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    rev, doc = project_json(c)
    sent = json.dumps({"cards": {"source": {"x": 1, "y": 2}}, "displays": {"source": {"x": 1, "y": 2, "width": 3, "height": 4}}},
                      separators=(",", ":"))
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="ui:x", changes=[
        change("SetProject", json=json.dumps(renamed_project(doc, "src", "source"))),
        change("RenameDefinition", id="src", to="source"),
        change("SetLayout", json=sent)])
    assert text(c.call("ProjectInspectRequest").layoutJson) == sent

    rev, doc = project_json(c)
    stale = {"cards": {"named": {"x": 9, "y": 9}, "source": {"x": 1, "y": 2}, "out": {"x": 3, "y": 4}}}
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetProject", json=json.dumps(renamed_project(doc, "source", "named"))),
        change("RenameDefinition", id="source", to="named"),
        change("SetLayout", json=json.dumps(stale))])
    assert json.loads(text(c.call("ProjectInspectRequest").layoutJson))["cards"] == {"named": {"x": 1, "y": 2}, "out": {"x": 3, "y": 4}}


def test_a_card_that_leaves_the_canvas_takes_its_layout_with_it(tmp_path, core_factory):
    """Removing a card also removes its position, frame and group membership.

    A new card with the same name starts without that layout, while restoring
    the card and its layout together keeps the supplied layout.
    """
    root = tmp_path / "p"
    laid_out_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    rev, doc = project_json(c)
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetProject", json=json.dumps(without_card(doc, "src"))), change("RemoveDefinition", id="src")])
    kept = json.loads(json.dumps(LAID_OUT))
    kept["cards"] = {k: v for k, v in kept["cards"].items() if k not in ("src", "src/inner")}
    del kept["displays"]["src"]
    kept["groups"][0]["cards"] = ["out"]
    kept["groups"][0]["outputs"] = [{"card": "out", "port": "x"}]
    assert text(c.call("ProjectInspectRequest").layoutJson) == spelled(kept)

    # Off the canvas with its definition left behind is deleted too.
    rev, doc = project_json(c)
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetProject", json=json.dumps(without_card(doc, "src2")))])
    del kept["cards"]["src2"]
    del kept["displays"]["src2"]
    assert text(c.call("ProjectInspectRequest").layoutJson) == spelled(kept)

    # A Card of the same name starts with no frame.
    rev, doc = project_json(c)
    doc["canvas"]["cards"].append({"id": "src"})
    res = c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("PutDefinition", id="src", cardJson=json.dumps({"template": "logger"})),
        change("SetProject", json=json.dumps(doc))])
    assert res.delta.layoutJson is None
    assert text(c.call("ProjectInspectRequest").layoutJson) == spelled(kept)

    # Nothing of the Card in the layout: not rewritten, even in another form.
    rev, doc = project_json(c)
    compact = json.dumps({"cards": {"out": {"x": 1, "y": 2}}}, separators=(",", ":"))
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetLayout", json=compact)])
    rev, doc = project_json(c)
    res = c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetProject", json=json.dumps(without_card(doc, "src"))), change("RemoveDefinition", id="src")])
    assert res.delta.layoutJson is None
    assert text(c.call("ProjectInspectRequest").layoutJson) == compact

    # An undo: the Card comes back, with its layout, in one save.
    rev, doc = project_json(c)
    doc["canvas"]["cards"].append({"id": "src"})
    back = pretty({"cards": {"src": {"x": 5, "y": 6}, "out": {"x": 1, "y": 2}}})
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="ui:x", changes=[
        change("PutDefinition", id="src", cardJson=json.dumps({"template": "logger"})),
        change("SetProject", json=json.dumps(doc)),
        change("SetLayout", json=back)])
    assert text(c.call("ProjectInspectRequest").layoutJson) == back


def test_every_card_needs_its_own_definition(tmp_path, core_factory):
    """A canvas cannot reference a missing or shared definition."""
    root = tmp_path / "p"
    builtin_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    rev, doc = project_json(c)
    doc["canvas"]["cards"].append({"id": "nobody"})
    with pytest.raises(RpcFailure) as err:
        c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t",
               changes=[change("SetProject", json=json.dumps(doc))])
    assert err.value.kind == "invalid_document"
    doc["canvas"]["cards"][-1] = {"id": "x", "definition": "src"}  # sharing is gone
    with pytest.raises(RpcFailure) as err:
        c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t",
               changes=[change("SetProject", json=json.dumps(doc))])
    assert err.value.kind == "invalid_document"


def test_card_in_card_is_an_external_project(tmp_path, core_factory):
    """A linked project can run as a nested card and as a standalone project."""
    root = tmp_path / "p"
    builtin_project(root)
    external(root, "greeting", "from the external")
    put(root / "definitions" / "greet" / "_link.json", json.dumps({"external": "greeting"}))
    project = json.loads((root / "tsugumi.json").read_text())
    project["canvas"]["cards"].append({"id": "greet"})
    project["canvas"]["cards"].append({"id": "show"})
    put(root / "definitions" / "show" / "_card.json", json.dumps({"template": "logger"}))
    project["canvas"]["connections"].append(conn("greet", "said", "show", "in"))
    put(root / "tsugumi.json", json.dumps(project))
    d = core_factory(project_root=root)
    c = d.connect()
    snap = c.call("ProjectInspectRequest")
    cards = {"/".join(text(s) for s in r.card.segments) for r in snap.resolution}
    assert {"greet", "greet/say", "greet/pass"} <= cards
    assert any(text(x.id) == "greet" and x.linkJson for x in snap.definitions)
    run_id, st = run_and_wait(c, "greet")
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    log = c.call("LogRangeRequest", runId=run_id, card=path("show"), invocation=1, stream=1, limitBytes=65536)
    assert "from the external" in text(log.text)
    # The external is this project's copy: its files change here like any other.
    rev = snap.rev
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t",
           changes=[change("PutSource", path="externals/greeting/notes.txt", bytes=list(b"x"))])
    assert bytes(c.call("ProjectFileReadRequest", path="externals/greeting/notes.txt").bytes) == b"x"
    snap = c.call("ProjectInspectRequest")
    c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
           changes=[change("PutSource", path="externals/fresh/tsugumi.json", bytes=list(b"{}"))])
    # The external opens on its own, with the same result.
    d.stop()
    alone = root / "externals" / "greeting"
    d = core_factory(project_root=alone)
    c = d.connect()
    run_id, st = run_and_wait(c, "say")
    assert st.state == RunState.succeeded


def test_link_diagnostics(tmp_path, core_factory):
    """Missing external cards are diagnosed and links outside the project are rejected."""
    root = tmp_path / "p"
    builtin_project(root)
    put(root / "definitions" / "missing" / "_link.json", json.dumps({"external": "nowhere"}))
    project = json.loads((root / "tsugumi.json").read_text())
    project["canvas"]["cards"].append({"id": "missing"})
    put(root / "tsugumi.json", json.dumps(project))
    c = core_factory(project_root=root).connect()
    assert "unknown_external" in diagnostics(c, ["missing"])
    before = c.call("ProjectInspectRequest")
    with pytest.raises(RpcFailure) as failure:
        c.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test", changes=[
            change("PutDefinition", id="bad", cardJson="", linkJson=json.dumps({"external": "../up"}))])
    assert failure.value.kind == "invalid_document"
    after = c.call("ProjectInspectRequest")
    assert after.rev == before.rev
    assert [text(d.id) for d in after.definitions] == [text(d.id) for d in before.definitions]


def test_trigger_setting(tmp_path, core_factory):
    """An unknown trigger setting is reported on the affected card."""
    root = tmp_path / "p"
    builtin_project(root)
    card = json.loads((root / "definitions" / "out" / "_card.json").read_text())
    card["trigger"] = "whenever"
    put(root / "definitions" / "out" / "_card.json", json.dumps(card))
    d = core_factory(project_root=root)
    c = d.connect()
    assert "invalid_setting" in diagnostics(c, ["out"])


def test_display_mode_setting(tmp_path, core_factory):
    """displayMode is update or live; anything else is reported on the Card."""
    for value, valid in (("update", True), ("live", True), ("smooth", False)):
        root = tmp_path / value
        builtin_project(root)
        card = json.loads((root / "definitions" / "out" / "_card.json").read_text())
        card["displayMode"] = value
        put(root / "definitions" / "out" / "_card.json", json.dumps(card))
        c = core_factory(project_root=root).connect()
        assert ("invalid_setting" not in diagnostics(c, ["out"])) == valid, value


def test_output_types_lists_and_rewrites_a_port(tmp_path, core_factory):
    """Output type edits preserve unrelated schema text and reject incompatible types."""
    root = tmp_path / "p"
    builtin_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    fbs = ("namespace demo;\n// kept\ntable Msg { text:string; }\ntable Path { value:string; }\n"
           "table Outputs { msg:Msg (required); /* note */ other:Msg; }\nroot_type Outputs;\n")
    res = c.call("OutputTypesRequest", text=fbs)
    assert text(res.rootType) == "demo.Outputs" and len(bytes(res.bfbs)) > 0 and res.text is None
    changed = c.call("OutputTypesRequest", text=fbs, port="msg", typeName="demo.Path", kind=1)  # File
    out = text(changed.text)
    assert 'msg:Path (required, tsugumi: "file");' in out and "other:Msg;" in out and "// kept" in out
    assert 'attribute "tsugumi";' in out
    bad = c.call("OutputTypesRequest", text=fbs, port="msg", typeName="demo.Missing")
    assert bad.text is None and "Missing" in text(bad.error)
    # A File port needs a string value: Msg has none.
    bad = c.call("OutputTypesRequest", text=fbs, port="msg", typeName="demo.Msg", kind=1)
    assert bad.text is None and bad.error


def test_attached_folder_is_read_only_inside_it(tmp_path, core_factory):
    """A folder attached to a chat: listed and read, never outside it."""
    root = tmp_path / "p"
    builtin_project(root)
    folder = tmp_path / "attached"
    (folder / "sub").mkdir(parents=True)
    (folder / "notes.txt").write_text("日本語のメモ")
    (tmp_path / "secret.txt").write_text("outside")
    (folder / "escape").symlink_to(tmp_path / "secret.txt")
    d = core_factory(project_root=root)
    c = d.connect()
    # Only folders the launcher listed as attached (a tsugumid on the host).
    with pytest.raises(RpcFailure) as err:
        c.call("AttachedFolderReadRequest", folder=str(folder), path="")
    assert err.value.kind == "not_found"
    (d.control / "attached.json").write_text(json.dumps({"folders": [str(folder)]}))
    with pytest.raises(RpcFailure) as err:
        c.call("AttachedFolderReadRequest", folder=str(tmp_path), path="secret.txt")
    assert err.value.kind == "not_found"
    listing = bytes(c.call("AttachedFolderReadRequest", folder=str(folder), path="").bytes).decode()
    assert listing.splitlines() == ["escape", "notes.txt", "sub/"]
    assert bytes(c.call("AttachedFolderReadRequest", folder=str(folder), path="notes.txt").bytes).decode() == "日本語のメモ"
    for bad in ("../secret.txt", "escape"):
        with pytest.raises(RpcFailure) as err:
            c.call("AttachedFolderReadRequest", folder=str(folder), path=bad)
        assert err.value.kind in ("invalid_path", "permission_denied"), bad


def test_rename_inside_a_card_in_card_keeps_the_history(tmp_path, core_factory):
    """Renaming a nested card makes its saved history available under the new name."""
    root = tmp_path / "p"
    builtin_project(root)
    external(root, "greeting", "hello")
    put(root / "definitions" / "greet" / "_link.json", json.dumps({"external": "greeting"}))
    project = json.loads((root / "tsugumi.json").read_text())
    project["canvas"]["cards"].append({"id": "greet"})
    put(root / "tsugumi.json", json.dumps(project))
    d = core_factory(project_root=root)
    c = d.connect()
    run_id, st = run_and_wait(c, "greet")
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    assert c.call("RunInvocationsRequest", runId=run_id, card=path("greet", "say")).records
    snap = c.call("ProjectInspectRequest")
    hashes = {text(s.path): text(s.hash) for s in snap.sources}
    ext = "externals/greeting/"
    doc = json.loads(bytes(c.call("ProjectFileReadRequest", path=ext + "tsugumi.json").bytes))
    doc["canvas"]["cards"] = [{"id": "talk" if x["id"] == "say" else x["id"]} for x in doc["canvas"]["cards"]]
    for e in doc["canvas"]["connections"]:
        for side in ("from", "to"):
            if e[side]["card"] == "say":
                e[side]["card"] = "talk"
    changes = [change("PutSource", path=ext + "tsugumi.json", bytes=list(json.dumps(doc).encode()), baseHash=hashes[ext + "tsugumi.json"])]
    for f in ("_card.json", "_outputs.fbs"):
        old = ext + "definitions/say/" + f
        changes.append(change("PutSource", path=ext + "definitions/talk/" + f, bytes=bytes(c.call("ProjectFileReadRequest", path=old).bytes)))
        changes.append(change("RemoveSource", path=old, baseHash=hashes[old]))
    from Tsugumi.RPC.CardRename import CardRenameT
    r = CardRenameT()
    r.from_ = path("greet", "say")
    r.to = path("greet", "talk")
    c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t", changes=changes, renamedCards=[r])
    assert c.call("RunInvocationsRequest", runId=run_id, card=path("greet", "talk")).records
    with pytest.raises(RpcFailure):
        c.call("RunInvocationsRequest", runId=run_id, card=path("greet", "say"))


def test_an_output_feeds_one_wire(tmp_path, core_factory):
    """Connecting one output twice is diagnosed; adding a fork makes the graph runnable."""
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("out2", "log")], extra_conns=[conn("src", "msg", "out2", "in")])
    d = core_factory(project_root=root)
    c = d.connect()
    assert "fan_out" in diagnostics(c, ["src"])
    # Through a fork, each output has one wire.
    rev, project = project_json(c)
    project["canvas"]["cards"].append({"id": "share"})
    project["canvas"]["connections"] = [
        conn("src", "msg", "share", "in"), conn("share", "out1", "out", "in"), conn("share", "out2", "out2", "in")]
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="test", changes=[
        change("PutDefinition", id="share", cardJson=json.dumps({"template": "fork"})),
        change("SetProject", json=json.dumps(project))])
    assert "fan_out" not in diagnostics(c, ["src"])
    _, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded
