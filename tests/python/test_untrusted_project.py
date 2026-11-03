"""A project folder may come from someone else: what tsugumid reads back from
its .tsugumi/ (the save journal, the copy of the run history) is checked
before it is used, so it cannot write, read or delete outside the project."""

from __future__ import annotations

import base64
import json
import shutil
import sqlite3
import struct
import zlib
from pathlib import Path

from conftest import Daemon
from projects import run_and_wait
from test_history import project
from test_run_history import copy_runs, listed, start, wait_copy
from tsugumi_client import RpcFailure, new_id, text


def journal_record(payload: dict) -> bytes:
    data = json.dumps(payload).encode()
    return b"TSJ1" + struct.pack("<II", len(data), zlib.crc32(data) & 0xFFFFFFFF) + data


def test_a_journal_record_writing_outside_the_project_is_not_applied(tmp_path):
    root = tmp_path / "p"
    project(root, False, False)
    d = Daemon(root).start()
    d.cleanup()
    outside = tmp_path / "outside.txt"
    state = root / ".tsugumi"
    targets = [str(outside), "../outside.txt", ".tsugumi/history.sqlite"]
    with open(state / "document.journal", "ab") as f:
        f.write(journal_record({
            "t": "txn", "rev": 99, "execution_rev": 99, "op": new_id(), "kind": "save", "digest": "", "receipt": "",
            "files": [{"p": p, "delete": False, "c": base64.b64encode(b"written by a journal").decode()} for p in targets]}))
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        snap = c.call("ProjectInspectRequest")
        assert snap.rev < 99
    finally:
        d.cleanup()
    assert not outside.exists()
    assert not (state / "history.sqlite").exists() or b"written by a journal" not in (state / "history.sqlite").read_bytes()


def tamper(copy: Path, sql: str) -> None:
    with sqlite3.connect(copy) as db:
        tables = [r[0] for r in db.execute("SELECT name FROM sqlite_master WHERE type='table'")]
        for t in tables:
            cols = [r[1] for r in db.execute(f"PRAGMA table_info({t})")]
            if "run_id" in cols:
                db.execute(sql.format(table=t))


def test_a_copied_history_names_no_folder_outside_the_project(tmp_path):
    root = tmp_path / "p"
    project(root, True, True)
    d = start(root, tmp_path / "h1")
    try:
        c = d.client()
        c.hello()
        good = [run_and_wait(c, "src")[0] for _ in range(2)]
        wait_copy(root, 2)
    finally:
        d.cleanup()
    # A copy of the project, its runs.sqlite edited: one run id is a path, and
    # the other run's artifact points outside its folder.
    victim = tmp_path / "victim"
    victim.mkdir()
    (victim / "keep.txt").write_text("keep")
    copy = tmp_path / "copy"
    shutil.copytree(root, copy)
    runs = copy / ".tsugumi" / "runs.sqlite"
    tamper(runs, f"UPDATE {{table}} SET run_id='../../../victim' WHERE run_id='{good[0]}'")
    with sqlite3.connect(runs) as db:
        members = db.execute("SELECT members FROM artifacts WHERE run_id=?", (good[1],)).fetchone()[0]
        m = json.loads(members)
        for item in m["members"]:
            item["path"] = "../../../../victim/keep.txt"
        db.execute("UPDATE artifacts SET members=? WHERE run_id=?", (json.dumps(m), good[1]))
    d = start(copy, tmp_path / "h2")
    try:
        c = d.client()
        c.hello()
        assert listed(c) == [good[1]]
        art = c.call("ArtifactListRequest", runId=good[1], limit=10).artifacts[0]
        try:
            c.call("ArtifactReadRequest", artifactId=text(art.artifactId))
            raise AssertionError("an artifact member outside its folder was read")
        except RpcFailure as e:
            assert e.kind == "not_found", e
        c.call("RunHistoryDeleteRequest", opId=new_id())
        assert listed(c) == []
    finally:
        d.cleanup()
    assert (victim / "keep.txt").read_text() == "keep"
    assert copy_runs(copy) == []
