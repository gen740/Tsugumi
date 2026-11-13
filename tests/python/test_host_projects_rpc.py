"""Host-side project lifecycle through the binary Host contract."""
from __future__ import annotations

import importlib
import re
import socket
import struct
import sys
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

from conftest import BUILD

AGENT = Path(os.environ.get("TSUGUMI_AGENTD", BUILD / "core/tsugumi-agentd"))
BINDINGS = Path(os.environ.get("TSUGUMI_PY_BINDINGS", BUILD / "generated/python"))
sys.path.insert(0, str(BINDINGS))
pytestmark = pytest.mark.skipif(not AGENT.exists() or not (BINDINGS / "Tsugumi/Host").exists(),
                                reason="needs built agentd and generated Host bindings")


@pytest.fixture
def host(tmp_path):
    demos = tmp_path / "demos"
    source = demos / "ja/sample"
    source.mkdir(parents=True)
    (source / "tsugumi.json").write_text('{"name":"Sample"}')
    (source / ".tsugumi").mkdir()
    (source / ".tsugumi/history").write_text("private")
    (demos / "ja/index.json").write_text(json.dumps({"demos": [
        {"id": "sample", "title": "Sample", "path": "sample", "field": ""}]}))
    with tempfile.TemporaryDirectory(prefix="tsg-host-") as short:
        state = tmp_path / "state"
        with (tmp_path / "agent.log").open("w") as log:
            proc = subprocess.Popen([str(AGENT), "--state-dir", str(state), "--unix", short + "/s",
                                     "--demos-dir", str(demos), "--no-websocket"], stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 10
            while not (state / "agent.ready").exists():
                assert proc.poll() is None, (tmp_path / "agent.log").read_text()
                assert time.monotonic() < deadline, "agentd startup timed out"
                time.sleep(0.02)
            ready = json.loads((state / "agent.ready").read_text())

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
                size, = struct.unpack("<I", read_exact(peer, 4))
                assert 0 < size <= 16 << 20
                data = read_exact(peer, size)
                assert Message.MessageBufferHasIdentifier(data, 0)
                reply = MessageT.InitFromPackedBuf(data)
                assert reply.requestId == i and reply.payloadType == Payload.Reply
                return reply.payload

            def view(value):
                if isinstance(value, bytes): return value.decode()
                if isinstance(value, list): return [view(item) for item in value]
                if not hasattr(value, "__dict__"): return value
                names = {"nixpkgsPin": "nixpkgsPin", "openedAt": "openedAt"}
                return {names.get(k, k): view(v) for k, v in vars(value).items() if v is not None}

            def call(method, fields=None, *, external=False):
                with socket.socket(socket.AF_UNIX) as peer:
                    peer.settimeout(10)
                    peer.connect(ready["host_socket"])
                    hello = exchange(peer, 1, "HelloRequest", {"client_id": "host-python-test", "protocol": 1,
                        "token": ready["external_token" if external else "user_token"]})
                    if hello.valueType == ReplyPayload.Failure: return {"code": hello.value.code.decode()}
                    assert hello.valueType == ReplyPayload.HelloResult
                    reply = exchange(peer, 2, method, fields or {})
                    if reply.valueType == ReplyPayload.Failure: return {"code": reply.value.code.decode()}
                    value = view(reply.value)
                    for key in ("projects", "demos", "models"):
                        if key in value: return value[key]
                    return value

            yield call, source, state
        finally:
            proc.terminate()
            proc.wait(timeout=10)
            for p in source.rglob("*"):
                p.chmod(0o755 if p.is_dir() else 0o644)
            source.chmod(0o755)


def test_project_requests_and_exactly_empty_destination_rule(host, tmp_path):
    call, _, _ = host
    defaults = call("HostProjectDefaultsRequest")
    folder = tmp_path / "project"
    folder.mkdir()
    made = call("HostProjectCreateRequest", {"parent": str(tmp_path), "name": "project", "flow": "leftToRight"})
    assert made == {"path": str(folder.resolve())}
    doc = json.loads((folder / "tsugumi.json").read_text())
    assert doc["pkgsPath"] == defaults["nixpkgsPin"]
    assert json.loads((folder / "layout.json").read_text()) == {"flow": "leftToRight"}
    assert call("HostProjectInfoRequest", {"path": str(folder)})["name"] == "project"
    assert call("HostProjectCreateRequest", {"parent": str(tmp_path), "name": "project"})["code"] == "folder_not_empty"
    assert call("HostProjectCreateRequest", {"parent": str(tmp_path), "name": "../escape"})["code"] == "invalid_project_name"
    assert call("HostProjectCreateRequest", {"parent": str(tmp_path), "name": "bad", "flow": "bad"})["code"] == "invalid_flow"
    assert not (tmp_path / "bad").exists()
    assert call("HostRecentListRequest", external=True)["code"] == "permission_denied"


def test_demo_and_duplicate_are_writable_and_drop_run_state(host, tmp_path):
    call, source, _ = host
    for p in source.rglob("*"):
        p.chmod(0o555 if p.is_dir() else 0o444)
    source.chmod(0o555)
    demo = call("HostDemoListRequest", {"language": "ja"})[0]
    assert demo["id"] == "sample" and demo["description"] == "" and demo["tags"] == []
    assert "field" not in demo
    assert call("HostDemoListRequest", {"language": "fr"})["code"] == "invalid_language"
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


def test_registry_limit_order_uniqueness_and_forget(host, tmp_path):
    call, source, state = host
    assert call("HostRecentListRequest") == []
    for i in range(32):
        folder = tmp_path / f"p-{i}"
        call("HostProjectCreateRequest", {"parent": str(tmp_path), "name": folder.name})
        call("HostRecentRememberRequest", {"path": str(folder)})
    entries = call("HostRecentListRequest")
    assert len(entries) == 30
    assert [e["name"] for e in entries] == [f"p-{i}" for i in range(31, 1, -1)]
    assert all(a["openedAt"] > b["openedAt"] for a, b in zip(entries, entries[1:]))
    entry = call("HostRecentRememberRequest", {"path": str(tmp_path / "p-5")})
    assert entry["name"] == "p-5" and entry["openedAt"] > 1_000_000_000_000
    assert len(call("HostRecentListRequest")) == 30
    call("HostRecentForgetRequest", {"paths": [entry["path"]]})
    assert (tmp_path / "p-5/tsugumi.json").exists()
    assert len(call("HostRecentListRequest")) == 29
    assert json.loads((state / "recent-projects.json").read_text()) == call("HostRecentListRequest")
