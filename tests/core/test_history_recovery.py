"""Explicit recovery preserves incompatible histories before starting a fresh database."""
import json
import sqlite3
import subprocess
import sys

import pytest
from harness.daemon import Daemon


def history_fixture(path, *, portable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(path) as db:
        db.execute("CREATE TABLE fixture(note TEXT)")
        db.execute("INSERT INTO fixture VALUES ('keep this evidence')")
        if portable:
            db.execute("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT)")
            db.executemany("INSERT INTO meta VALUES (?,?)", [("format", "tsugumi-runs"), ("version", "999")])
        else:
            db.execute("PRAGMA user_version=999")
    return path.read_bytes()


@pytest.mark.parametrize("external", [False, True], ids=["project", "vm-disk"])
@pytest.mark.parametrize("portable", [False, True], ids=["local", "portable"])
def test_incompatible_history_requires_explicit_archive(project, tmp_path, external, portable):
    history = tmp_path / "vm-history" if external else project / ".tsugumi"
    database = project / ".tsugumi" / "runs.sqlite" if portable else history / "history.sqlite"
    original = history_fixture(database, portable=portable)
    daemon = Daemon(project, history_dir=history)
    try:
        with pytest.raises(RuntimeError, match="schema version 999"):
            daemon.start()
        marker = json.loads((daemon.control / "server.startup-error.json").read_text())
        assert marker["kind"] == "incompatible_history"
        assert marker["found"] == "999"
        assert marker["source"] == ("portable" if portable else "local")
        assert database.read_bytes() == original
        daemon.args.append("--archive-incompatible-history")
        daemon.start()
        assert not (daemon.control / "server.startup-error.json").exists()
        archives = list(database.parent.glob("history-archive-*/" + database.name))
        assert len(archives) == 1
        assert archives[0].read_bytes() == original
        daemon.connect().call("ProjectInspectRequest")
    finally:
        daemon.close()


def test_recovery_validates_both_histories_before_archiving(project, tmp_path):
    history = tmp_path / "vm-history"
    local = history / "history.sqlite"
    original = history_fixture(local)
    copy = project / ".tsugumi" / "runs.sqlite"
    copy.parent.mkdir(exist_ok=True)
    copy.write_bytes(b"not a sqlite database")
    daemon = Daemon(project, history_dir=history, args=["--archive-incompatible-history"])
    try:
        with pytest.raises(RuntimeError):
            daemon.start()
        assert local.read_bytes() == original
        assert copy.read_bytes() == b"not a sqlite database"
        assert not list(history.glob("history-archive-*"))
        assert not (daemon.control / "server.startup-error.json").exists()
    finally:
        daemon.close()


def test_explicit_recovery_preserves_both_incompatible_databases(project, tmp_path):
    history = tmp_path / "vm-history"
    local = history / "history.sqlite"
    copy = project / ".tsugumi" / "runs.sqlite"
    originals = [(local, history_fixture(local)), (copy, history_fixture(copy, portable=True))]
    daemon = Daemon(project, history_dir=history, args=["--archive-incompatible-history"])
    try:
        daemon.start()
        for database, original in originals:
            archives = list(database.parent.glob("history-archive-*/" + database.name))
            assert len(archives) == 1
            assert archives[0].read_bytes() == original
    finally:
        daemon.close()


def history_with_wal(database):
    database.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, "-c", """
import os, sqlite3, sys
c = sqlite3.connect(sys.argv[1])
c.execute('PRAGMA journal_mode=WAL')
c.execute('PRAGMA wal_autocheckpoint=0')
c.execute('CREATE TABLE fixture(note TEXT)')
c.execute("INSERT INTO fixture VALUES ('WAL evidence')")
c.execute('PRAGMA user_version=999')
c.commit()
os._exit(0)
""", str(database)], check=True)
    wal = database.with_name(database.name + "-wal")
    return database.read_bytes(), wal.read_bytes()


def test_recovery_keeps_uncheckpointed_wal(project, tmp_path):
    """Archive the committed WAL alongside its database rather than losing its last records."""
    history = tmp_path / "vm-history"
    database = history / "history.sqlite"
    original_db, original_wal = history_with_wal(database)
    daemon = Daemon(project, history_dir=history, args=["--archive-incompatible-history"])
    try:
        daemon.start()
        archive = next(history.glob("history-archive-*/history.sqlite"))
        assert archive.read_bytes() == original_db
        assert archive.with_name("history.sqlite-wal").read_bytes() == original_wal
        with sqlite3.connect(f"file:{archive}?mode=ro", uri=True) as db:
            assert db.execute("SELECT note FROM fixture").fetchone() == ("WAL evidence",)
    finally:
        daemon.close()



def test_interrupted_archive_blocks_fresh_start_and_resumes_existing_intent(project, tmp_path):
    """A crash after moving the main DB leaves its committed WAL available for retry."""
    history = tmp_path / "vm-history"
    database = history / "history.sqlite"
    wal = history / "history.sqlite-wal"
    intent = history / "history.sqlite.archive-pending"
    original_db, original_wal = history_with_wal(database)
    archive_flag = "--archive-incompatible-history"
    daemon = Daemon(project, history_dir=history, args=[archive_flag], fault="archive_after_main_move")
    try:
        with pytest.raises(RuntimeError, match=r"daemon exited \(97\)"):
            daemon.start()
        destination = history / ("history-archive-" + intent.read_text()) / database.name
        assert not database.exists()
        assert destination.read_bytes() == original_db
        assert wal.read_bytes() == original_wal

        daemon.fault = None
        daemon.args.remove(archive_flag)
        with pytest.raises(RuntimeError, match="unfinished archive"):
            daemon.start()
        marker = json.loads((daemon.control / "server.startup-error.json").read_text())
        assert marker["kind"] == "incompatible_history" and marker["found"] == "pending"
        assert marker["source"] == "local"
        assert not database.exists()
        assert wal.read_bytes() == original_wal

        daemon.args.append(archive_flag)
        daemon.start()
        assert not intent.exists()
        assert not (daemon.control / "server.startup-error.json").exists()
        assert list(history.glob("history-archive-*")) == [destination.parent]
        assert destination.read_bytes() == original_db
        assert destination.with_name(wal.name).read_bytes() == original_wal
        with sqlite3.connect(f"file:{destination}?mode=ro", uri=True) as db:
            assert db.execute("SELECT note FROM fixture").fetchone() == ("WAL evidence",)
        daemon.connect().call("ProjectInspectRequest")
    finally:
        daemon.close()


@pytest.mark.parametrize("suffix", ["", "-wal"])
def test_recovery_checks_history_files_before_sqlite_opens_them(project, tmp_path, suffix):
    history = tmp_path / "vm-history"
    history.mkdir()
    database = history / "history.sqlite"
    if suffix:
        history_fixture(database)
    target = tmp_path / "outside.sqlite"
    suspect = history / (database.name + suffix)
    suspect.symlink_to(target)
    daemon = Daemon(project, history_dir=history, args=["--archive-incompatible-history"])
    try:
        with pytest.raises(RuntimeError, match="not a regular file"):
            daemon.start()
        assert suspect.is_symlink()
        assert not target.exists()
        assert not list(history.glob("history-archive-*"))
    finally:
        daemon.close()


def test_recovery_rejects_conflicting_source_and_archive_destination(project, tmp_path):
    history = tmp_path / "vm-history"
    database = history / "history.sqlite"
    original = history_fixture(database)
    archive_id = "47ad62f7-3f7d-471a-b3a3-28bcb99de812"
    intent = history / "history.sqlite.archive-pending"
    intent.write_text(archive_id)
    destination = history / f"history-archive-{archive_id}" / database.name
    destination.parent.mkdir()
    destination.write_bytes(b"preexisting archive evidence")
    daemon = Daemon(project, history_dir=history, args=["--archive-incompatible-history"])
    try:
        with pytest.raises(RuntimeError, match="both source and destination"):
            daemon.start()
        assert database.read_bytes() == original
        assert destination.read_bytes() == b"preexisting archive evidence"
        assert intent.read_text() == archive_id
    finally:
        daemon.close()
