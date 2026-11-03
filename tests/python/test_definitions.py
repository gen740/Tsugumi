"""One definition per Card and Card in Card as externals: a Card's name is its
id and its definition folder; renaming moves the folder and its run history;
Card in Card is definitions/<id>/_link.json to externals/<p>/, a project that
also opens on its own (its boundary in its own tsugumi.json, its own
definitions and pkgsPath, its .tsugumi never read), read-only from the
outer project except while it is being created; trigger replaces inputMode."""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import pytest

from conftest import Daemon
from projects import MSG_FBS, builtin_project, conn
from test_regressions import put, run_and_wait
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
    # Its own state is never read from here.
    put(ext / ".tsugumi" / "history.sqlite", "not a database")


def test_rename_moves_the_definition_and_its_history(tmp_path):
    root = tmp_path / "p"
    builtin_project(root, text="hi")
    put(root / "definitions" / "src" / "notes.txt", "kept")
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
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
        assert (root / "definitions" / "source" / "notes.txt").read_text() == "kept"
        assert not (root / "definitions" / "src").exists()
        # The run's records follow the new name.
        rec = c.call("RunRecordRequest", runId=run_id, card=path("source"))
        assert text(rec.record.summary.card.segments[0]) == "source"
        assert c.call("RunInvocationsRequest", runId=run_id, card=path("source")).records
        run2, st2 = run_and_wait(c, "source")
        assert st2.state == RunState.succeeded
    finally:
        d.cleanup()


def test_every_card_needs_its_own_definition(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
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
    finally:
        d.cleanup()


def test_card_in_card_is_an_external_project(tmp_path):
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
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        snap = c.call("ProjectInspectRequest")
        cards = {"/".join(text(s) for s in r.card.segments) for r in snap.resolution}
        assert {"greet", "greet/say", "greet/pass"} <= cards
        assert any(text(x.id) == "greet" and x.linkJson for x in snap.definitions)
        assert not any(text(s.path).startswith("externals/greeting/.tsugumi") for s in snap.sources)
        run_id, st = run_and_wait(c, "greet")
        assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
        log = c.call("LogRangeRequest", runId=run_id, card=path("show"), invocation=1, stream=1, limitBytes=65536)
        assert "from the external" in text(log.text)
        # The external is read-only here; a new one can be written.
        rev = snap.rev
        with pytest.raises(RpcFailure) as err:
            c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t",
                   changes=[change("PutSource", path="externals/greeting/notes.txt", bytes=list(b"x"))])
        assert err.value.kind == "permission_denied"
        c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t",
               changes=[change("PutSource", path="externals/fresh/tsugumi.json", bytes=list(b"{}"))])
    finally:
        d.cleanup()
    # The external opens on its own, with the same result.
    alone = tmp_path / "alone"
    shutil.copytree(root / "externals" / "greeting", alone, ignore=shutil.ignore_patterns(".tsugumi"))
    d = Daemon(alone).start()
    try:
        c = d.client()
        c.hello()
        run_id, st = run_and_wait(c, "say")
        assert st.state == RunState.succeeded
    finally:
        d.cleanup()


def test_link_diagnostics(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    put(root / "definitions" / "missing" / "_link.json", json.dumps({"external": "nowhere"}))
    put(root / "definitions" / "bad" / "_link.json", json.dumps({"external": "../up"}))
    project = json.loads((root / "tsugumi.json").read_text())
    project["canvas"]["cards"].append({"id": "missing"})
    put(root / "tsugumi.json", json.dumps(project))
    d = Daemon(root)
    try:
        # A link outside externals/ is not a project at all.
        d.start(expect_exit=True)
        assert d.proc.poll() is not None
    finally:
        d.cleanup()
    shutil.rmtree(root / "definitions" / "bad")
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        assert "unknown_external" in diagnostics(c, ["missing"])
    finally:
        d.cleanup()


def test_trigger_setting(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    card = json.loads((root / "definitions" / "out" / "_card.json").read_text())
    card["trigger"] = "whenever"
    put(root / "definitions" / "out" / "_card.json", json.dumps(card))
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        assert "invalid_setting" in diagnostics(c, ["out"])
    finally:
        d.cleanup()


def test_output_types_lists_and_rewrites_a_port(tmp_path):
    """The UI never parses fbs: the core compiles _outputs.fbs (bfbs) and
    rewrites one port's type / kind, leaving the rest of the text alone."""
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
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
    finally:
        d.cleanup()


def test_attached_folder_is_read_only_inside_it(tmp_path):
    """A folder attached to a chat: listed and read, never outside it."""
    root = tmp_path / "p"
    builtin_project(root)
    folder = tmp_path / "attached"
    (folder / "sub").mkdir(parents=True)
    (folder / "notes.txt").write_text("日本語のメモ")
    (tmp_path / "secret.txt").write_text("outside")
    (folder / "escape").symlink_to(tmp_path / "secret.txt")
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        listing = bytes(c.call("AttachedFolderReadRequest", folder=str(folder), path="").bytes).decode()
        assert listing.splitlines() == ["escape", "notes.txt", "sub/"]
        assert bytes(c.call("AttachedFolderReadRequest", folder=str(folder), path="notes.txt").bytes).decode() == "日本語のメモ"
        for bad in ("../secret.txt", "escape"):
            with pytest.raises(RpcFailure) as err:
                c.call("AttachedFolderReadRequest", folder=str(folder), path=bad)
            assert err.value.kind in ("invalid_path", "permission_denied"), bad
    finally:
        d.cleanup()
