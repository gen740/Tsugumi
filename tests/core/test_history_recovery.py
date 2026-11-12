"""Explicit recovery preserves incompatible histories before starting a fresh database."""
import json
import sqlite3

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


def test_recovery_keeps_uncheckpointed_wal(project, tmp_path):
    """Archive the committed WAL alongside its database rather than losing its last records."""
    import subprocess
    import sys
    history = tmp_path / "vm-history"
    history.mkdir()
    database = history / "history.sqlite"
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
    original_db = database.read_bytes()
    original_wal = database.with_name("history.sqlite-wal").read_bytes()
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
