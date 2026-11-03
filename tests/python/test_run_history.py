"""Run history on its own disk (--history-dir, as in the shared VM) and the
project's copy of finished runs (.tsugumi/runs.sqlite): written shortly after
runs end, read back by a copied project, merged by run id, switched off by
saveRunHistory, deleted together with the runs' files."""

from __future__ import annotations

import hashlib

import json
import shutil
import sqlite3
import time
from pathlib import Path

from conftest import Daemon
from test_history import BIG, project, read_all
from projects import run_and_wait
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState


def start(root: Path, history: Path) -> Daemon:
    return Daemon(root, args=["--history-dir", str(history)]).start()


def copy_runs(root: Path) -> list[str]:
    f = root / ".tsugumi" / "runs.sqlite"
    if not f.exists():
        return []
    with sqlite3.connect(f"file:{f}?mode=ro", uri=True) as db:
        return [r[0] for r in db.execute("SELECT run_id FROM runs ORDER BY seq")]


def wait_copy(root: Path, n: int, timeout: float = 20.0) -> list[str]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        runs = copy_runs(root)
        if len(runs) >= n:
            return runs
        time.sleep(0.1)
    raise TimeoutError(f"{len(copy_runs(root))} of {n} runs in .tsugumi/runs.sqlite")


def listed(c) -> list[str]:
    return [text(r.status.runId) for r in c.call("RunHistoryRequest", limit=100).runs]


def test_copy_is_written_and_read_back_by_a_copied_project(tmp_path):
    root = tmp_path / "p"
    project(root, True, True)
    d = start(root, tmp_path / "h1")
    try:
        c = d.client()
        c.hello()
        ids = [run_and_wait(c, "src")[0] for _ in range(2)]
        assert wait_copy(root, 2) == ids
    finally:
        d.cleanup()
    # The database itself is not in the project.
    assert (tmp_path / "h1" / "history.sqlite").exists()
    assert not (root / ".tsugumi" / "history.sqlite").exists()
    with sqlite3.connect(root / ".tsugumi" / "runs.sqlite") as db:
        assert dict(db.execute("SELECT key, value FROM meta")) == {"format": "tsugumi-runs", "version": "2"}
        # Values stay FlatBuffers bytes, typed by their schema's bfbs, with their SHA-256.
        rows = db.execute("SELECT v.bytes, v.hash, s.bfbs FROM ivalues v JOIN schemas s ON s.hash = v.schema_hash").fetchall()
        assert rows and all(b and bfbs and h == hashlib.sha256(b).hexdigest() for b, h, bfbs in rows)
        assert db.execute("SELECT count(*) FROM logs").fetchone()[0] > 0
        assert db.execute("SELECT count(*) FROM artifacts").fetchone()[0] == 2
        assert db.execute("PRAGMA journal_mode").fetchone()[0] == "delete"

    # A copy of the project starts with an empty history disk and reads the runs back.
    other = tmp_path / "copy"
    shutil.copytree(root, other, symlinks=True)
    d = start(other, tmp_path / "h2")
    try:
        c = d.client()
        c.hello()
        assert listed(c) == list(reversed(ids))
        rec = c.call("RunRecordRequest", runId=ids[0], card=path("src"))
        assert len(rec.record.outputs) == 1
        arts = c.call("ArtifactListRequest", runId=ids[0], card=path("src"), limit=100).artifacts
        assert read_all(c, "ArtifactReadRequest", 65536, artifactId=text(arts[0].artifactId)).decode() == BIG
        # A run here joins the copy's runs.sqlite.
        new = run_and_wait(c, "src")[0]
        assert wait_copy(other, 3)[-1] == new
    finally:
        d.cleanup()

    # Merged by run id: the original keeps its two and gains the copy's new run.
    shutil.copy(other / ".tsugumi" / "runs.sqlite", root / ".tsugumi" / "runs.sqlite")
    d = start(root, tmp_path / "h1")
    try:
        c = d.client()
        c.hello()
        assert sorted(listed(c)) == sorted(ids + [new])
    finally:
        d.cleanup()


def test_save_run_history_off_and_on(tmp_path):
    root = tmp_path / "p"
    project(root, True, False)
    doc = json.loads((root / "tsugumi.json").read_text())
    doc["saveRunHistory"] = False
    (root / "tsugumi.json").write_text(json.dumps(doc))
    d = start(root, tmp_path / "h")
    try:
        c = d.client()
        c.hello()
        first = run_and_wait(c, "src")[0]
        time.sleep(3)
        assert copy_runs(root) == []
        # Switched on: the copy catches up with the runs made meanwhile.
        doc["saveRunHistory"] = True
        snap = c.call("ProjectInspectRequest")
        c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
               changes=[change("SetProject", json=json.dumps(doc))])
        assert wait_copy(root, 1) == [first]
    finally:
        d.cleanup()


def test_delete_runs_here_in_the_copy_and_their_files(tmp_path):
    root = tmp_path / "p"
    project(root, True, True)
    d = start(root, tmp_path / "h")
    try:
        c = d.client()
        c.hello()
        runs = [run_and_wait(c, "src") for _ in range(3)]
        ids = [r for r, _ in runs]
        wait_copy(root, 3)
        for r in ids:
            assert (root / ".tsugumi" / "runs" / r).is_dir()
        # Older than the third run: the first two go, everywhere.
        op = new_id()
        res = c.call("RunHistoryDeleteRequest", opId=op, endedBefore=runs[1][1].endedAt + 1)
        assert res.deletedRuns == 2 and res.freedBytes > 0
        assert listed(c) == [ids[2]]
        assert copy_runs(root) == [ids[2]]
        assert not (root / ".tsugumi" / "runs" / ids[0]).exists()
        assert (root / ".tsugumi" / "runs" / ids[2]).is_dir()
        try:
            c.call("RunRecordRequest", runId=ids[0], card=path("src"))
            raise AssertionError("deleted run still readable")
        except RpcFailure:
            pass
        # The same operation again answers from its receipt.
        assert c.call("RunHistoryDeleteRequest", opId=op, endedBefore=runs[1][1].endedAt + 1).deletedRuns == 2
        assert c.call("RunHistoryDeleteRequest", opId=new_id()).deletedRuns == 1
        assert listed(c) == [] and copy_runs(root) == []
    finally:
        d.cleanup()
    # Nothing comes back on the next start.
    d = start(root, tmp_path / "h")
    try:
        c = d.client()
        c.hello()
        assert listed(c) == []
    finally:
        d.cleanup()


def test_a_history_of_another_layout_is_started_over(tmp_path):
    root = tmp_path / "p"
    project(root, True, True)
    hist = tmp_path / "h"
    hist.mkdir()
    # An earlier layout (no version, an older ivalues without hash).
    with sqlite3.connect(hist / "history.sqlite") as db:
        db.execute("CREATE TABLE ivalues(run_id TEXT, bytes BLOB)")
        db.execute("INSERT INTO ivalues VALUES('old', x'00')")
    d = start(root, hist)
    try:
        c = d.client()
        c.hello()
        rid, _ = run_and_wait(c, "src")
        assert listed(c) == [rid]
    finally:
        d.cleanup()
    with sqlite3.connect(hist / "history.sqlite") as db:
        assert db.execute("PRAGMA user_version").fetchone()[0] == 2
        assert "hash" in [r[1] for r in db.execute("PRAGMA table_info(ivalues)")]
