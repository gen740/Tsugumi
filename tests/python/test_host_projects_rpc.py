"""Host-side project lifecycle through the binary Host contract."""
from __future__ import annotations

import fcntl
import http.client
import importlib
import re
import socket
import sqlite3
import struct
import sys
import json
import os
import subprocess
import tempfile
from contextlib import contextmanager
from pathlib import Path
from urllib.parse import urlsplit

from harness.file_events import process_file

import pytest

from conftest import BUILD, EMPTY_PROJECT, write_project

HOST = Path(os.environ.get("TSUGUMI_HOSTD", BUILD / "hostd/tsugumi-hostd"))
BINDINGS = Path(os.environ.get("TSUGUMI_PY_BINDINGS", BUILD / "generated/python"))
sys.path.insert(0, str(BINDINGS))
pytestmark = pytest.mark.skipif(not HOST.exists() or not (BINDINGS / "Tsugumi/Host").exists(),
                                reason="needs built hostd and generated Host bindings")


@contextmanager
def running_host(args, publication, log_path):
    with log_path.open("w") as log:
        proc = subprocess.Popen([str(HOST), *args], stdout=subprocess.DEVNULL, stderr=log)
    try:
        assert process_file(proc, publication) is not None, "hostd did not publish startup state"
        yield proc
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
                raise


def test_ensure_reports_the_live_holder_when_readiness_is_missing(tmp_path):
    state = tmp_path / "state"
    ready = state / "host.ready"
    with tempfile.TemporaryDirectory(prefix="tsugumi-host-") as short:
        args = ["--state-dir", str(state), "--unix", short + "/s",
                "--socket-dir", short + "/b", "--no-websocket"]
        with running_host([*args, "--application-name", "Tsugumi Dev"], ready, tmp_path / "holder.log") as holder:
            reopened = subprocess.run([str(HOST), "--ensure", *args], stdout=subprocess.DEVNULL,
                                      stderr=subprocess.PIPE, timeout=10)
            assert reopened.returncode == 0
            ready.unlink()
            blocked = subprocess.run([str(HOST), "--ensure", *args], capture_output=True, text=True, timeout=10)
            assert blocked.returncode == 1
            error = json.loads(blocked.stdout)
            assert error["code"] == "host_state_locked"
            assert error["lock_holder"] == {"pid": holder.pid, "application": "Tsugumi Dev"}
            assert "Quit it and retry" in error["message"]
            assert "pid" not in error
            assert holder.poll() is None
            assert not (state / "host.startup-error").exists()
        # A copied or stale record can even name a live PID. Without the
        # record's OS lock it does not identify whoever holds host.lock.
        (state / "host.lock-owner").write_text(json.dumps({
            "pid": os.getpid(), "application": "Stale", "hostname": socket.gethostname(),
        }))
        with (state / "host.lock").open("r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            blocked = subprocess.run([str(HOST), "--ensure", *args], capture_output=True, text=True, timeout=10)
            assert blocked.returncode == 1
            error = json.loads(blocked.stdout)
            assert error["code"] == "host_state_locked"
            assert "lock_holder" not in error
            assert "Stale" not in error["message"]
        with running_host(args, ready, tmp_path / "restarted.log"):
            assert ready.exists()
        assert not ready.exists()


@pytest.mark.parametrize("recovering", [False, True])
def test_old_host_exit_preserves_replacement_publication_and_sockets(tmp_path, recovering):
    state = tmp_path / "state"
    web = tmp_path / "web"
    web.mkdir()
    (web / "index.html").write_text("<!doctype html>")
    publication = state / ("host.startup-error" if recovering else "host.ready")

    def prepare_state():
        if recovering:
            state.mkdir()
            with sqlite3.connect(state / "execution.sqlite") as db:
                db.execute("CREATE TABLE stale(value)")

    def identity(path):
        st = path.stat()
        return st.st_dev, st.st_ino

    prepare_state()
    with tempfile.TemporaryDirectory(prefix="tsugumi-host-") as short:
        args = ["--state-dir", str(state), "--unix", short + "/s",
                "--socket-dir", short + "/b", "--no-websocket"]
        if recovering:
            args += ["--web-root", str(web), "--web-port", "0"]
        moved = tmp_path / "moved"
        with running_host(args, publication, tmp_path / "original.log") as original:
            state.rename(moved)
            prepare_state()
            with running_host(args, publication, tmp_path / "replacement.log") as replacement:
                paths = [publication]
                if not recovering:
                    paths += [Path(short) / "s", Path(short) / "s.host",
                              Path(short) / "b/s", Path(short) / "b/s.c"]
                before = [identity(path) for path in paths]
                original.terminate()
                assert original.wait(timeout=10) == int(recovering)
                assert replacement.poll() is None
                assert [identity(path) for path in paths] == before
                for path in paths[1:]:
                    # The replacement listeners still accept connections.
                    with socket.socket(socket.AF_UNIX) as peer:
                        peer.settimeout(3)
                        peer.connect(str(path))
            assert not publication.exists()


@pytest.fixture
def host(tmp_path):
    demos = tmp_path / "demos"
    source = demos / "ja/sample"
    source.mkdir(parents=True)
    write_project(source, {"name": "Sample"})
    (source / ".tsugumi").mkdir()
    (source / ".tsugumi/history").write_text("private")
    (demos / "ja/index.json").write_text(json.dumps({"demos": [
        {"id": "sample", "title": "Sample", "path": "sample", "field": ""}]}))
    with tempfile.TemporaryDirectory(prefix="tsugumi-host-") as short:
        state = tmp_path / "state"
        with (tmp_path / "host.log").open("w") as log:
            proc = subprocess.Popen([str(HOST), "--state-dir", str(state), "--unix", short + "/s",
                                     "--socket-dir", short + "/b", "--usage-scratch-dir", str(tmp_path.resolve() / "scratch"),
                                     "--demos-dir", str(demos), "--no-websocket"], stdout=log, stderr=log)
        try:
            published = process_file(proc, state / "host.ready")
            assert published is not None, (tmp_path / "host.log").read_text()
            ready = json.loads(published.read_text())

            import flatbuffers
            from Tsugumi.Host.Message import Message, MessageT
            from Tsugumi.Host.Request import RequestT
            from Tsugumi.Host.RequestPayload import RequestPayload
            from Tsugumi.Host.ReplyPayload import ReplyPayload
            from Tsugumi.Host.Payload import Payload

            def read_exact(peer, count):
                data = b""
                while len(data) < count:
                    part = peer.recv(count - len(data))
                    assert part, "Host connection closed"
                    data += part
                return data

            def exchange(peer, i, method, fields):
                ns = "Service" if method == "HelloRequest" else "Host"
                table = getattr(importlib.import_module(f"Tsugumi.{ns}.{method}"), method + "T")()
                for key, value in fields.items():
                    setattr(table, re.sub(r"_([a-z])", lambda m: m[1].upper(), key), value)
                request = RequestT(valueType=getattr(RequestPayload, method), value=table)
                envelope = MessageT(requestId=i, payloadType=Payload.Request, payload=request)
                builder = flatbuffers.Builder()
                builder.Finish(envelope.Pack(builder), file_identifier=b"TSH1")
                data = builder.Output()
                peer.sendall(struct.pack("<I", len(data)) + data)
                while True:
                    size, = struct.unpack("<I", read_exact(peer, 4))
                    assert 0 < size <= 16 << 20
                    data = read_exact(peer, size)
                    assert Message.MessageBufferHasIdentifier(data, 0)
                    reply = MessageT.InitFromPackedBuf(data)
                    if reply.requestId == 0 and reply.payloadType == Payload.Event:
                        continue
                    assert reply.requestId == i and reply.payloadType == Payload.Reply
                    return reply.payload

            def view(value):
                if isinstance(value, bytes): return value.decode()
                if isinstance(value, list): return [view(item) for item in value]
                if not hasattr(value, "__dict__"): return value
                names = {"openedAt": "openedAt"}
                return {names.get(k, k): view(v) for k, v in vars(value).items() if v is not None}

            def call(method, fields=None, *, external=False):
                with socket.socket(socket.AF_UNIX) as peer:
                    peer.settimeout(10)
                    peer.connect(ready["host_socket"])
                    hello = exchange(peer, 1, "HelloRequest", {"token": ready["external_token" if external else "user_token"]})
                    if hello.valueType == ReplyPayload.Failure: return {"code": hello.value.code.decode()}
                    assert hello.valueType == ReplyPayload.HelloResult
                    reply = exchange(peer, 2, method, fields or {})
                    if reply.valueType == ReplyPayload.Failure: return {"code": reply.value.code.decode()}
                    value = view(reply.value)
                    # An empty vector is absent from the unpacked reply.
                    lists = {"HostRecentListRequest": "projects", "HostDemoListRequest": "demos", "ModelsListRequest": "models"}
                    return value.get(lists[method], []) if method in lists else value

            yield call, source, state
        finally:
            proc.terminate()
            proc.wait(timeout=10)
            for p in source.rglob("*"):
                p.chmod(0o755 if p.is_dir() else 0o644)
            source.chmod(0o755)




def test_creation_writes_all_project_values(host, tmp_path):
    call, _, _ = host
    defaults = call("HostProjectDefaultsRequest")
    created = Path(call("HostProjectCreateRequest", {
        "parent": str(tmp_path), "name": "Fresh Project",
    })["path"])
    assert json.loads((created / "tsugumi.json").read_text()) == {
        **EMPTY_PROJECT, "name": "Fresh Project", "agentExclude": defaults["agentExclude"],
    }


def test_demo_and_duplicate_are_writable_and_drop_run_state(host, tmp_path):
    call, source, _ = host
    for p in source.rglob("*"):
        p.chmod(0o555 if p.is_dir() else 0o444)
    source.chmod(0o555)
    demo = call("HostDemoListRequest", {"type": "code", "language": "ja"})[0]
    assert demo["id"] == "sample" and demo["description"] == "" and demo.get("tags", []) == []
    assert "field" not in demo
    assert call("HostDemoListRequest", {"type": "code", "language": "fr"})["code"] == "invalid_language"
    assert call("HostDemoCopyRequest", {"parent": str(tmp_path), "id": "missing"})["code"] == "demo_not_found"
    for method, fields in (
        ("HostDemoCopyRequest", {"id": "sample"}),
        ("HostProjectDuplicateRequest", {"path": str(source), "name": "duplicate"}),
    ):
        copied = Path(call(method, {"parent": str(tmp_path), **fields})["path"])
        assert not (copied / ".tsugumi").exists()
        assert (copied / "tsugumi.json").stat().st_mode & 0o200
        (copied / "tsugumi.json").write_text('{"name":"editable"}')
        (copied / "new-directory").mkdir()
    assert Path(call("HostDemoCopyRequest", {"parent": str(tmp_path), "id": "sample"})["path"]).name == "sample-2"


def test_host_state_recovery_preserves_an_old_execution_store(tmp_path):
    state = tmp_path / "state"
    state.mkdir()
    store = state / "execution.sqlite"
    # An unclean exit leaves the committed row only in the WAL.
    subprocess.run([sys.executable, "-c", """
import os, sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.executescript("PRAGMA journal_mode=WAL; CREATE TABLE evidence(value); INSERT INTO evidence VALUES('keep')")
os._exit(0)
""", str(store)], check=True)
    wal = state / "execution.sqlite-wal"
    assert wal.is_file()
    web = tmp_path / "web"
    web.mkdir()
    (web / "index.html").write_text("<!doctype html>")
    with tempfile.TemporaryDirectory(prefix="tsugumi-host-") as short:
        with (tmp_path / "host.log").open("w") as log:
            proc = subprocess.Popen([
                str(HOST), "--ensure", "--state-dir", str(state), "--unix", short + "/s",
                "--socket-dir", short + "/b", "--web-root", str(web), "--web-port", "0",
                "--no-websocket",
            ], stdout=subprocess.PIPE, stderr=log)
        try:
            diagnostic = process_file(proc, state / "host.startup-error")
            assert diagnostic is not None, (tmp_path / "host.log").read_text()
            issue = json.loads(diagnostic.read_text())
            assert issue["state_format_path"] == str(store.resolve())
            assert issue["message"].startswith("This state was written by a different Tsugumi version.")
            assert issue["found_format"] == 0
            assert wal.is_file()
            assert not list(state.glob("*.stale-*"))
            url = urlsplit(issue["recovery_url"])
            peer = http.client.HTTPConnection(url.hostname, url.port, timeout=10)

            def request(method, path, headers=None):
                peer.request(method, path, headers=headers or {})
                response = peer.getresponse()
                return response, response.read()

            try:
                response, _ = request("GET", f"/?{url.query}")
                assert response.status == 302
                cookie = response.getheader("Set-Cookie").split(";", 1)[0]
                headers = {"Cookie": cookie, "Origin": f"http://{url.netloc}",
                           "X-Tsugumi-Recovery": issue["recovery_token"]}
                endpoint = "/api/host/startup-error"
                assert request("POST", endpoint, {**headers, "Origin": "http://example.org"})[0].status == 403
                assert request("GET", endpoint, headers)[0].status == 200
                assert request("POST", endpoint, headers)[0].status == 200
            finally:
                peer.close()
            assert process_file(proc, state / "host.ready") is not None, (tmp_path / "host.log").read_text()
            fresh = sqlite3.connect(store)
            try:
                assert fresh.execute("PRAGMA user_version").fetchone()[0] > 0
                assert "worker_capabilities" in {row[1] for row in fresh.execute("PRAGMA table_info(services)")}
            finally:
                fresh.close()
            backup, = [entry for entry in state.glob("execution.sqlite.stale-*")
                       if not entry.name.endswith(("-wal", "-shm"))]
            assert Path(f"{backup}-wal").is_file()
            original = sqlite3.connect(f"{backup.as_uri()}?mode=ro", uri=True)
            try:
                assert original.execute("SELECT value FROM evidence").fetchall() == [("keep",)]
                assert original.execute("PRAGMA user_version").fetchone() == (0,)
            finally:
                original.close()
        finally:
            proc.terminate()
            proc.wait(timeout=10)
            proc.stdout.close()


@pytest.mark.parametrize(("name", "kind"), [
    ("agent.sqlite", "incompatible_state"),
    ("document.sqlite", "incompatible_state"),
    ("runs.sqlite", "incompatible_history"),
])
def test_project_state_recovery_requires_an_explicit_request(host, tmp_path, name, kind):
    from Tsugumi.Host.EnvironmentAction import EnvironmentAction
    from Tsugumi.Host.EnvironmentPhase import EnvironmentPhase

    call, _, _ = host
    project = tmp_path / "project"
    project.mkdir()
    write_project(project, {"name": "Project"})
    folder = project / ".tsugumi"
    folder.mkdir()
    store = folder / name
    with sqlite3.connect(store) as db:
        db.executescript("CREATE TABLE evidence(value); INSERT INTO evidence VALUES('keep')")
    if name == "runs.sqlite":
        artifact = folder / "artifacts/8"
        artifact.mkdir(parents=True)
        (artifact / "saved").write_text("keep")
    if name == "document.sqlite":
        staged = folder / "document-files"
        staged.mkdir()
        (staged / "saved").write_text("keep")
    fields = {"project": str(project)}
    result = call("EnvironmentRequest", {**fields, "action": EnvironmentAction.Start})
    issue = result["startupError"]
    assert issue["kind"] == kind
    assert issue["path"] == str(store.resolve())
    assert store.is_file()
    assert not list(folder.glob("*.stale-*"))
    recover = {**fields, "action": EnvironmentAction.MoveStateAside, "stale_store": issue["path"]}
    assert "code" in call("EnvironmentRequest", recover, external=True)
    assert store.is_file()
    assert "code" not in call("EnvironmentRequest", recover)
    assert not store.exists()
    assert "code" in call("EnvironmentRequest", recover)
    moved, = folder.glob(f"{name}.stale-*")
    with sqlite3.connect(f"{moved.as_uri()}?mode=ro", uri=True) as db:
        assert db.execute("SELECT value FROM evidence").fetchall() == [("keep",)]
    result = call("EnvironmentRequest", {**fields, "action": EnvironmentAction.Start})
    assert result["phase"] == EnvironmentPhase.Ready, result
    if name != "agent.sqlite":
        with sqlite3.connect(store) as db:
            assert db.execute("PRAGMA user_version").fetchone()[0] > 0
            if name == "runs.sqlite":
                assert db.execute("SELECT value FROM run_counter").fetchone() == (8,)
                assert (artifact / "saved").read_text() == "keep"
    if name == "document.sqlite":
        backup, = folder.glob("document-files.stale-*")
        assert (backup / "saved").read_text() == "keep"
