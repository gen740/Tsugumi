"""Logger sinks through public RPC, including permissions and real SQL databases."""
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess

import pytest

from projects import builtin_project, run_and_wait
from harness.permissions import permissions, save
from tsugumi_client import RpcFailure, change, path, text
from Tsugumi.Data.RunState import RunState


def project(root, url, *, network=False, table="measurements"):
    builtin_project(root, text='a quote: " and SQL: \'; DROP TABLE measurements; --')
    body = json.loads((root / "tsugumi.json").read_text())
    body["databaseConnections"] = [{"name": "metrics", "url": url}]
    (root / "tsugumi.json").write_text(json.dumps(body))
    (root / "definitions/out/_card.json").write_text(json.dumps({
        "template": "logger", "databaseConnection": "metrics", "databaseTable": table, "network": network}))
    return body


def test_sqlite_rows_are_traceable_and_json_is_bound(tmp_path, core_factory):
    """Two runs persist independently with run/Card/invocation/time and exact JSON values."""
    root = tmp_path / "p"
    project(root, "sqlite:readings.sqlite")
    c = core_factory(project_root=root).connect()
    runs = []
    for _ in range(2):
        rid, status = run_and_wait(c, "src")
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        runs.append(rid)
    with sqlite3.connect(root / ".tsugumi/logger/readings.sqlite") as db:
        rows = db.execute("SELECT run_id, card, invocation, timestamp_ms, value_json FROM measurements").fetchall()
    assert {r[0] for r in rows} == set(runs)
    assert all(r[1] == "out" and r[2] == 1 and r[3] > 0 for r in rows)
    for row in rows:
        retained = c.call("RunRecordRequest", runId=row[0], card=path("out")).record.display.value
        assert json.loads(row[4]) == json.loads(text(retained.text))
        assert "DROP TABLE" in row[4]


def test_sqlite_failure_fails_invocation_and_logs_reason(tmp_path, core_factory):
    """A conflicting table fails the logger, preserving the DB rather than overwriting it."""
    root = tmp_path / "p"
    project(root, "sqlite:readings.sqlite")
    folder = root / ".tsugumi/logger"
    folder.mkdir(parents=True)
    with sqlite3.connect(folder / "readings.sqlite") as db:
        db.execute("CREATE TABLE measurements (unexpected TEXT)")
    c = core_factory(project_root=root).connect()
    rid, status = run_and_wait(c, "src")
    assert status.state == RunState.failed
    log = c.call("LogRangeRequest", runId=rid, card=path("out"), invocation=1, stream=2)
    assert "SQLite write failed" in text(log.text)


def test_sqlite_cannot_follow_a_database_symlink(tmp_path, core_factory):
    root = tmp_path / "p"
    project(root, "sqlite:readings.sqlite")
    folder = root / ".tsugumi/logger"
    folder.mkdir(parents=True)
    outside = tmp_path / "outside.sqlite"
    outside.write_text("keep me")
    (folder / "readings.sqlite").symlink_to(outside)
    c = core_factory(project_root=root).connect()
    _, status = run_and_wait(c, "src")
    assert status.state == RunState.failed
    assert outside.read_text() == "keep me"


@pytest.mark.parametrize("url", ["postgresql://user:secret@localhost/data", "postgresql://localhost/data?password=secret",
                                  "postgresql://localhost/data?service=other", "sqlite:../secret.sqlite", "sqlite:history"])
def test_secret_urls_and_escaping_paths_never_enter_documents(tmp_path, core_factory, url):
    root = tmp_path / "p"
    body = project(root, "sqlite:readings.sqlite")
    c = core_factory(project_root=root).connect()
    before = (root / "tsugumi.json").read_bytes()
    body["databaseConnections"][0]["url"] = url
    with pytest.raises(RpcFailure):
        save(c, [change("SetProject", json=json.dumps(body))], origin="ui")
    assert (root / "tsugumi.json").read_bytes() == before


def test_agent_approval_covers_destination_table_and_network(tmp_path, core_factory):
    root = tmp_path / "p"
    body = project(root, "sqlite:readings.sqlite")
    c = core_factory(project_root=root).connect()
    update = {"template": "logger", "databaseConnection": "metrics", "databaseTable": "other"}
    changes = [change("PutDefinition", id="out", cardJson=json.dumps(update), keepMeta=True)]
    with pytest.raises(RpcFailure) as failure:
        save(c, changes)
    assert permissions(failure.value) == ["database_write out metrics sqlite:readings.sqlite other"]
    save(c, changes, approved=permissions(failure.value))
    body["databaseConnections"][0]["url"] = "postgresql://127.0.0.1/metrics?sslmode=disable"
    update["network"] = True
    changes = [change("SetProject", json=json.dumps(body)),
               change("PutDefinition", id="out", cardJson=json.dumps(update), keepMeta=True)]
    with pytest.raises(RpcFailure) as failure:
        save(c, changes)
    assert set(permissions(failure.value)) == {
        "network out", "database_write out metrics postgresql://127.0.0.1/metrics?sslmode=disable other"}


@pytest.fixture
def postgres(tmp_path):
    """A private, password-authenticated PostgreSQL instance; never a user's server."""
    for tool in ["initdb", "pg_ctl", "psql"]:
        if not shutil.which(tool):
            pytest.skip("PostgreSQL tools are supplied by nix develop")
    if os.geteuid() == 0:
        pytest.skip("PostgreSQL initdb requires a non-root test user")
    directory = tmp_path / "postgres"
    pwfile = tmp_path / "postgres-password"
    password = "logger-secret-not-for-project-743"
    pwfile.write_text(password)
    pwfile.chmod(0o600)
    subprocess.run(["initdb", "-D", str(directory), "-U", "logger", "--auth-host=scram-sha-256",
                    "--auth-local=trust", "--pwfile", str(pwfile)], check=True, capture_output=True)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    subprocess.run(["pg_ctl", "-D", str(directory), "-l", str(tmp_path / "postgres.log"),
                    "-o", f"-h 127.0.0.1 -p {port} -k {tmp_path}", "-w", "start"], check=True, capture_output=True)
    try:
        yield f"postgresql://127.0.0.1:{port}/postgres?sslmode=disable", password
    finally:
        subprocess.run(["pg_ctl", "-D", str(directory), "-m", "immediate", "-w", "stop"], check=True, capture_output=True)


def test_postgres_records_json_and_never_records_password(tmp_path, core_factory, postgres):
    url, password = postgres
    root = tmp_path / "p"
    project(root, url, network=True)
    credentials = tmp_path / "credentials"
    credentials.mkdir(mode=0o700)
    secret = credentials / "metrics.json"
    secret.write_text(json.dumps({"url": url, "user": "logger", "password": password}))
    secret.chmod(0o600)
    c = core_factory(project_root=root, args=["--database-credentials-dir", str(credentials)]).connect()
    rid, status = run_and_wait(c, "src")
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    result = subprocess.run(["psql", url, "-U", "logger", "-At", "-c",
                             "SELECT json_build_array(run_id,card,invocation,timestamp_ms,value_json) FROM measurements"],
                            env={**os.environ, "PGPASSWORD": password}, check=True, capture_output=True, text=True)
    row = json.loads(result.stdout)
    assert row[0:3] == [rid, "out", 1] and row[3] > 0 and "DROP TABLE" in row[4]["text"]
    for file in root.rglob("*"):
        if file.is_file():
            assert password.encode() not in file.read_bytes(), file
    # A permissive file, wrong target, or missing credential cannot be used.
    secret.chmod(0o644)
    _, status = run_and_wait(c, "src")
    assert status.state == RunState.failed
    secret.chmod(0o600)
    secret.write_text(json.dumps({"url": url + "x", "user": "logger", "password": password}))
    _, status = run_and_wait(c, "src")
    assert status.state == RunState.failed
    secret.unlink()
    _, status = run_and_wait(c, "src")
    assert status.state == RunState.failed


def test_postgres_without_network_cannot_run(tmp_path, core_factory):
    root = tmp_path / "p"
    project(root, "postgresql://127.0.0.1/postgres?sslmode=disable")
    c = core_factory(project_root=root).connect()
    try:
        _, status = run_and_wait(c, "src")
        assert status.state == RunState.failed
    except RpcFailure as failure:
        assert failure.kind == "validation_failed"


def test_postgres_credentials_in_project_are_rejected(tmp_path, core_factory):
    root = tmp_path / "p"
    url = "postgresql://127.0.0.1/postgres?sslmode=disable"
    project(root, url, network=True)
    directory = root / "credentials"
    directory.mkdir()
    credential = directory / "metrics.json"
    credential.write_text(json.dumps({"url": url, "user": "u", "password": "must-not-be-read"}))
    credential.chmod(0o600)
    c = core_factory(project_root=root, args=["--database-credentials-dir", str(directory)]).connect()
    rid, status = run_and_wait(c, "src")
    assert status.state == RunState.failed
    logs = text(c.call("LogRangeRequest", runId=rid, card=path("out"), invocation=1, stream=2).text)
    assert "outside the project" in logs and "must-not-be-read" not in logs


def test_nested_logger_uses_its_own_connections_and_requests_approval(tmp_path, core_factory):
    root = tmp_path / "p"
    body = project(root, "sqlite:outer.sqlite")
    inner = root / "externals/inner"
    project(inner, "sqlite:inner.sqlite")
    link = root / "definitions/inner"
    link.mkdir()
    (link / "_link.json").write_text('{"external":"inner"}')
    body["canvas"] = {"cards": [{"id": "inner"}], "connections": []}
    (root / "tsugumi.json").write_text(json.dumps(body))
    c = core_factory(project_root=root).connect()
    rid, status = run_and_wait(c, "inner/src", timeout=10)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert not (root / ".tsugumi/logger/outer.sqlite").exists()
    with sqlite3.connect(inner / ".tsugumi/logger/inner.sqlite") as db:
        assert db.execute("SELECT run_id, card FROM measurements").fetchone() == (rid, "inner/out")
    rel = "externals/inner/definitions/out/_card.json"
    setting = json.loads((root / rel).read_text())
    setting["databaseTable"] = "another"
    hashes = {text(s.path): text(s.hash) for s in c.call("ProjectInspectRequest").sources}
    with pytest.raises(RpcFailure) as failure:
        save(c, [change("PutSource", path=rel, bytes=list(json.dumps(setting).encode()), baseHash=hashes[rel])])
    assert permissions(failure.value) == ["database_write inner/out metrics sqlite:inner.sqlite another"]
