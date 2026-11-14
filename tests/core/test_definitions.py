"""Definition identity, nested projects and layout are observed through public RPC."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from harness.values import recorded_input_json
from projects import MSG_FBS, SINK, builtin_project, card, conn, put, run_and_wait
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
    """externals/<name>: input -> fork, with the forwarded message as the boundary output."""
    ext = root / "externals" / name
    put(ext / "tsugumi.json", json.dumps({
        "name": name, "pkgsPath": "tsugumi:default",
        "canvas": {"cards": [{"id": "say"}, {"id": "pass"}], "connections": [conn("say", "msg", "pass", "in")]},
        "boundary": {"inputs": {}, "outputs": {"said": {"card": "pass", "port": "out1"}}}}))
    put(ext / "layout.json", "{}")
    put(ext / "definitions" / "say" / "_card.json", json.dumps(
        {"template": "input", "params": [{"name": "text", "type": "string", "data": text_value},
                                         {"name": "value", "type": "bool", "data": True}]}))
    put(ext / "definitions" / "say" / "_outputs.fbs", MSG_FBS)
    put(ext / "definitions" / "pass" / "_card.json", json.dumps({"template": "fork", "params": [
        {"name": "branches", "type": "string[]", "data": ["out1"]}]}))


def rename(c, card_id, name, prefix=""):
    """Change a human name through the same metadata used by both UIs."""
    snap = c.call("ProjectInspectRequest")
    if not prefix:
        d = next(d for d in snap.definitions if text(d.id) == card_id)
        meta = json.loads(text(d.metaJson) or "{}")
        meta["name"] = name
        changes = [change("PutDefinition", id=card_id, cardJson=text(d.cardJson),
                          linkJson=text(d.linkJson), metaJson=json.dumps(meta))]
    else:
        filename = prefix + "definitions/" + card_id + "/_meta.json"
        hashes = {text(s.path): text(s.hash) for s in snap.sources}
        old = json.loads(bytes(c.call("ProjectFileReadRequest", path=filename).bytes)) if filename in hashes else {}
        old["name"] = name
        changes = [change("PutSource", path=filename, bytes=json.dumps(old).encode(), baseHash=hashes.get(filename))]
    return c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="agent", changes=changes)


def database_records(filename):
    """Every stored record, including JSON input sources and absolute file paths."""
    import sqlite3
    with sqlite3.connect(filename) as db:
        return "\n".join(db.iterdump()).encode()


def test_rename_preserves_every_past_record_and_saved_input(tmp_path, core_factory):
    """A name edit leaves all recorded bytes and exact addresses untouched."""
    root = tmp_path / "p"
    builtin_project(root, text="hi")
    put(root / "definitions" / "out" / "_card.json", json.dumps(dict(SINK, recordIO=True)))
    put(root / "definitions" / "src" / "notes.txt", "kept")
    c = core_factory(project_root=root).connect()
    run_id, st = run_and_wait(c, "src")
    assert st.state == RunState.succeeded
    snap = c.call("ProjectInspectRequest")
    before = database_records(root / ".tsugumi/history.sqlite")
    result = rename(c, "src", "日本語 / source")
    assert result.executionRev == snap.executionRev
    assert database_records(root / ".tsugumi/history.sqlite") == before
    assert text(c.call("ProjectInspectRequest").projectJson) == text(snap.projectJson)
    assert bytes(c.call("ProjectFileReadRequest", path="definitions/src/notes.txt").bytes) == b"kept"
    assert c.call("RunRecordRequest", runId=run_id, card=path("src")).record
    assert c.call("RunInvocationsRequest", runId=run_id, card=path("src")).records
    run2, st2 = run_and_wait(c, "out", propagate=False)
    assert st2.state == RunState.succeeded
    assert recorded_input_json(c, tmp_path / "decode", run2, "out")["text"] == "hi"


def test_new_ids_are_unique_and_names_may_be_shared(tmp_path, core_factory):
    """The core reserves salted ids, including before any definitions are saved."""
    import re
    root = tmp_path / "p"
    builtin_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    ids = [text(c.call("CardIdRequest", stem="aggregate").id) for _ in range(80)]
    assert len(set(ids)) == len(ids)
    assert all(re.fullmatch(r"aggregate-[abcdefghjkmnpqrstuvwxyz23456789]{4}", x) for x in ids)
    snap = c.call("ProjectInspectRequest")
    doc = json.loads(text(snap.projectJson))
    doc["canvas"]["cards"] += [{"id": x} for x in ids[:2]]
    c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="ui:test", changes=[
        change("SetProject", json=json.dumps(doc)), *[
            change("PutDefinition", id=x, cardJson='{"template":"draft"}', metaJson='{"name":"Aggregate"}') for x in ids[:2]]])
    snap = c.call("ProjectInspectRequest")
    assert sum(json.loads(text(x.metaJson) or "{}").get("name") == "Aggregate" for x in snap.definitions) == 2
    assert "src" in [text(x.id) for x in snap.definitions]  # existing ids are unchanged
    d.restart()
    c = d.connect()
    assert text(c.call("CardIdRequest", stem="aggregate").id) not in ids
    nested = text(c.call("CardIdRequest", stem="aggregate", projectPath="externals/new/").id)
    assert re.fullmatch(r"aggregate-[abcdefghjkmnpqrstuvwxyz23456789]{4}", nested)
    for bad in ("../", "externals/../", "definitions/a/", "externals/new"):
        with pytest.raises(RpcFailure):
            c.call("CardIdRequest", stem="aggregate", projectPath=bad)


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
    builtin_project(root, extra_cards=[card("src2", "sink")])
    (root / "layout.json").write_text(spelled(layout), encoding="utf-8")


def without_card(doc: dict, id: str) -> dict:
    doc["canvas"]["cards"] = [c for c in doc["canvas"]["cards"] if c["id"] != id]
    doc["canvas"]["connections"] = [e for e in doc["canvas"]["connections"] if id not in (e["from"]["card"], e["to"]["card"])]
    return doc


def test_an_agent_name_change_preserves_layout_verbatim(tmp_path, core_factory):
    """Names do not affect positions, frames, groups or any formatting."""
    root = tmp_path / "p"
    laid_out_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    result = rename(c, "src", "source")
    assert text(c.call("ProjectInspectRequest").layoutJson) == spelled(LAID_OUT)
    assert result.delta.layoutJson is None
    d.restart()
    assert text(d.connect().call("ProjectInspectRequest").layoutJson) == spelled(LAID_OUT)


def test_the_saving_client_supplies_deleted_card_layout_cleanup(tmp_path, core_factory):
    """The saving client supplies position, frame and group cleanup atomically.

    A new card with the same name starts without that layout, while restoring
    the card and its layout together keeps the supplied layout.
    """
    root = tmp_path / "p"
    laid_out_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    rev, doc = project_json(c)
    kept = json.loads(json.dumps(LAID_OUT))
    kept["cards"] = {k: v for k, v in kept["cards"].items() if k not in ("src", "src/inner")}
    del kept["displays"]["src"]
    kept["groups"][0]["cards"] = ["out"]
    kept["groups"][0]["outputs"] = [{"card": "out", "port": "x"}]
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetProject", json=json.dumps(without_card(doc, "src"))), change("RemoveDefinition", id="src"), change("SetLayout", json=spelled(kept))])
    assert text(c.call("ProjectInspectRequest").layoutJson) == spelled(kept)

    # Off the canvas with its definition left behind is deleted too.
    rev, doc = project_json(c)
    del kept["cards"]["src2"]
    del kept["displays"]["src2"]
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetProject", json=json.dumps(without_card(doc, "src2"))), change("SetLayout", json=spelled(kept))])
    assert text(c.call("ProjectInspectRequest").layoutJson) == spelled(kept)

    # A Card of the same name starts with no frame.
    rev, doc = project_json(c)
    doc["canvas"]["cards"].append({"id": "src"})
    res = c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("PutDefinition", id="src", cardJson=json.dumps(SINK)),
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
        change("PutDefinition", id="src", cardJson=json.dumps(SINK)),
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
    put(root / "definitions" / "show" / "_card.json", json.dumps(dict(SINK, recordIO=True)))
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
    assert recorded_input_json(c, tmp_path / "decode", run_id, "show")["text"] == "from the external"
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
    """Presentation modes do not participate in core validation."""
    for value, reuse_worker, valid in (("update", False, True), ("live", True, True), ("smooth", True, True)):
        root = tmp_path / value
        builtin_project(root)
        card = json.loads((root / "definitions" / "out" / "_card.json").read_text())
        card["displayMode"] = value
        card["reuseWorker"] = reuse_worker
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


def test_name_changes_inside_a_card_in_card_preserve_history(tmp_path, core_factory):
    """Both container and inner names change without touching a past record."""
    root = tmp_path / "p"
    builtin_project(root)
    external(root, "greeting", "hello")
    put(root / "definitions/greet/_link.json", '{"external":"greeting"}')
    doc = json.loads((root / "tsugumi.json").read_text())
    doc["canvas"]["cards"].append({"id": "greet"})
    put(root / "tsugumi.json", json.dumps(doc))
    c = core_factory(project_root=root).connect()
    run_id, st = run_and_wait(c, "greet")
    assert st.state == RunState.succeeded
    snap = c.call("ProjectInspectRequest")
    before = database_records(root / ".tsugumi/history.sqlite")
    rename(c, "say", "Greeting", "externals/greeting/")
    rename(c, "greet", "Greeting")
    assert database_records(root / ".tsugumi/history.sqlite") == before
    assert c.call("RunInvocationsRequest", runId=run_id, card=path("greet", "say")).records
    after = c.call("ProjectInspectRequest")
    assert after.executionRev == snap.executionRev
    assert ("greet", "say") in {tuple(text(s) for s in r.card.segments) for r in after.resolution}
    assert json.loads(bytes(c.call("ProjectFileReadRequest", path="externals/greeting/definitions/say/_meta.json").bytes))["name"] == "Greeting"


def test_an_output_feeds_one_wire(tmp_path, core_factory):
    """Connecting one output twice is diagnosed; adding a fork makes the graph runnable."""
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("out2", "sink")], extra_conns=[conn("src", "msg", "out2", "in")])
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
