"""An incompatible primary database stays available for the person's judgment."""
import sqlite3

import pytest

from harness.daemon import Daemon, StartupFailure


def test_incompatible_history_is_preserved(project):
    folder = project / ".tsugumi"
    folder.mkdir(exist_ok=True)
    database = folder / "runs.sqlite"
    with sqlite3.connect(database) as connection:
        connection.executescript("CREATE TABLE evidence(value TEXT); INSERT INTO evidence VALUES('retained'); PRAGMA user_version=999")

    with pytest.raises(StartupFailure) as failure:
        with Daemon(project):
            pass
    assert failure.value.kind == "incompatible_history"
    assert failure.value.record["found"] == "999"
    assert int(failure.value.record["expected"]) != 999
    assert failure.value.record["source"] == "local"
    with sqlite3.connect(f"{database.as_uri()}?mode=ro", uri=True) as connection:
        assert connection.execute("PRAGMA user_version").fetchone() == (999,)
        assert connection.execute("SELECT value FROM evidence").fetchall() == [("retained",)]
