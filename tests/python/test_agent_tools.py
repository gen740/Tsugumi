"""What the in-app Agent relies on: reads without at_rev use the latest
revision, "" names the project folder, project_validate checks proposed
changes without saving them, and an input Card hands a project file to the
next Card as a File."""

from __future__ import annotations

import json
from pathlib import Path

from conftest import Daemon
from projects import put, run_and_wait
from tsugumi_client import change, new_id, path, text
from Tsugumi.Data.RunState import RunState

FILE_FBS = 'attribute "tsugumi";\nnamespace data;\ntable Events { value:string; }\n' \
           'table Outputs { events:Events (required, tsugumi: "file"); }\nroot_type Outputs;\n'


def make(root: Path) -> None:
    put(root / "tsugumi.json", json.dumps({
        "name": "files", "pkgsPath": "tsugumi:default", "databaseConnections": [],
        "canvas": {"cards": [{"id": "start"}, {"id": "load"}, {"id": "show"}], "connections": [
            {"from": {"card": "start", "port": "out"}, "to": {"card": "load", "port": "start"}},
            {"from": {"card": "load", "port": "events"}, "to": {"card": "show", "port": "in"}}]}}))
    put(root / "layout.json", "{}")
    put(root / "definitions" / "start" / "_card.json", json.dumps({"template": "trigger"}))
    put(root / "definitions" / "load" / "_card.json", json.dumps({
        "template": "input", "params": [{"name": "value", "type": "string", "data": "definitions/load/data/events.txt"}]}))
    put(root / "definitions" / "load" / "_outputs.fbs", FILE_FBS)
    put(root / "definitions" / "load" / "data" / "events.txt", "photon 1\nphoton 2\n")
    put(root / "definitions" / "show" / "_card.json", json.dumps({"template": "logger"}))


def test_reads_without_at_rev_and_the_root_as_empty_path(tmp_path):
    root = tmp_path / "p"
    make(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        listed = c.call("ProjectFileListRequest", path="", depth=1, includeDirectories=True)
        names = {text(e.path) for e in listed.entries}
        assert "tsugumi.json" in names and "definitions" in names, names
        got = c.call("ProjectFileReadRequest", path="definitions/load/data/events.txt")
        assert bytes(got.bytes).decode().startswith("photon 1")
    finally:
        d.cleanup()


def test_validate_checks_proposed_changes_without_saving(tmp_path):
    root = tmp_path / "p"
    make(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        before = c.call("ProjectInspectRequest")
        bad = change("PutSource", path="definitions/load/_outputs.fbs", bytes=list(b"table Broken {"),
                     baseHash=next(text(s.hash) for s in before.sources if text(s.path) == "definitions/load/_outputs.fbs"))
        v = c.call("ProjectValidateRequest", targets=[path("load")], changes=[bad])
        assert not v.valid and v.diagnostics, "a broken _outputs.fbs is reported"
        after = c.call("ProjectInspectRequest")
        assert after.rev == before.rev, "nothing was saved"
        assert (root / "definitions" / "load" / "_outputs.fbs").read_text() == FILE_FBS
        # The current document itself is valid.
        assert c.call("ProjectValidateRequest").valid
    finally:
        d.cleanup()


def test_input_card_hands_a_project_file_on(tmp_path):
    root = tmp_path / "p"
    make(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        # A single Card run before any run says how to get its inputs.
        _, st = run_and_wait(c, "show", propagate=False)
        assert st.state == RunState.failed and "trigger" in text(st.failure.diagnostic), text(st.failure.diagnostic)
        rid, st = run_and_wait(c, "start")
        assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
        log = text(c.call("LogRangeRequest", runId=rid, card=path("show"), invocation=1, stream=1, limitBytes=65536).text)
        shown = json.loads(log)["value"]
        assert shown.endswith("/events.txt") and "definitions/load/data" not in shown, shown
    finally:
        d.cleanup()


def test_put_definition_can_keep_the_meta(tmp_path):
    """Settings change without writing the intent (_meta.json) again."""
    root = tmp_path / "p"
    make(root)
    meta = {"description": "観測データを読む", "content": "露光補正はしない"}
    put(root / "definitions" / "load" / "_meta.json", json.dumps(meta, ensure_ascii=False))
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        snap = c.call("ProjectInspectRequest")
        card = {"template": "input", "params": [{"name": "value", "type": "string", "data": "definitions/load/data/events.txt"}],
                "maxRuns": 2}
        c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
               changes=[change("PutDefinition", id="load", cardJson=json.dumps(card), keepMeta=True)])
        assert json.loads((root / "definitions" / "load" / "_meta.json").read_text()) == meta
        assert json.loads((root / "definitions" / "load" / "_card.json").read_text())["maxRuns"] == 2
    finally:
        d.cleanup()
