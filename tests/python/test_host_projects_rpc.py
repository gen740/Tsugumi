"""Host-side project lifecycle through agentd's shipped JSON-lines RPC client."""
from __future__ import annotations

import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

import pytest

from conftest import BUILD

AGENT = Path(os.environ.get("TSUGUMI_AGENTD", BUILD / "core/tsugumi-agentd"))
CLIENT = Path(os.environ.get("TSUGUMI_RPC_CLIENT", BUILD / "client/tsugumi-rpc"))
SCHEMA = Path(os.environ.get("TSUGUMI_AGENT_SCHEMA", BUILD / "generated/bfbs/agent.bfbs"))
pytestmark = pytest.mark.skipif(not all(p.exists() for p in (AGENT, CLIENT, SCHEMA)),
                                reason="needs built agentd, RPC client and agent.bfbs")


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

            def call(method, fields=None, *, external=False):
                def request(i, name, value):
                    return {"request_id": i, "payload_type": "Request", "payload": {
                        "value_type": name, "value": value}}
                hello = request(1, "HelloRequest", {"client_id": "host-python-test", "protocol": 1,
                    "token": ready["external_token" if external else "user_token"]})
                result = subprocess.run([str(CLIENT), "--endpoint", "unix:" + ready["unix_socket"],
                                         "--schema", str(SCHEMA), "--subprotocol", "tsugumi.agent.v1"],
                                        input=json.dumps(hello) + "\n" + json.dumps(request(2, method, fields or {})) + "\n",
                                        text=True, capture_output=True, timeout=10, check=True)
                reply = next(json.loads(line)["payload"] for line in result.stdout.splitlines()
                             if json.loads(line).get("request_id") == 2)
                if reply["value_type"] == "Failure":
                    return {"code": reply["value"]["code"]}
                return json.loads(reply["value"]["json"])

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
