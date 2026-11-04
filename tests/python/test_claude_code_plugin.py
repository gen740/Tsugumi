"""The Claude Code plugin's MCP server (web/dist/mcp.js): it starts the web UI
host when none runs (found again through its server.json), opens a project,
and runs the chat Agent's core tools there; a page sees each call (the bird
flies to the Card) and nothing works before a project is open. Needs node, a
built web/dist (npm run build) and tsugumid."""

from __future__ import annotations

import http.client
import json
import os
import shutil
import signal
import stat
import subprocess
import threading
import time
from pathlib import Path

import pytest

from conftest import TSUGUMID, write_project

ROOT = Path(__file__).resolve().parents[2]
MCP = ROOT / "web" / "dist" / "mcp.js"

pytestmark = pytest.mark.skipif(not shutil.which("node") or not MCP.exists() or not TSUGUMID.exists(),
                                reason="needs node, web/dist (cd web && npm run build) and tsugumid")


class MCPServer:
    def __init__(self, data: Path, cwd: Path):
        env = dict(os.environ, TSUGUMI_WEB_DATA=str(data), CLAUDE_PROJECT_DIR=str(cwd))
        self.p = subprocess.Popen(["node", str(MCP)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
        self.next = 0

    def request(self, method: str, params: dict | None = None) -> dict:
        self.next += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.next, "method": method, "params": params or {}}) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        assert line, "the MCP server ended"
        m = json.loads(line)
        assert m["id"] == self.next, m
        return m

    def call(self, name: str, arguments: dict) -> tuple[str, bool]:
        r = self.request("tools/call", {"name": name, "arguments": arguments})["result"]
        return r["content"][0]["text"], bool(r.get("isError"))

    def close(self):
        self.p.stdin.close()
        self.p.wait(10)


class Events(threading.Thread):
    """A page: the server's event stream."""

    def __init__(self, port: int, cookie: str):
        super().__init__(daemon=True)
        self.c = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
        self.c.request("GET", "/api/events", headers={"Host": f"127.0.0.1:{port}", "Cookie": cookie})
        self.r = self.c.getresponse()
        assert self.r.status == 200
        self.events: list[dict] = []

    def run(self):
        try:
            for raw in self.r:
                line = raw.decode().strip()
                if line.startswith("data:"):
                    self.events.append(json.loads(line[5:]))
        except Exception:
            pass


@pytest.fixture
def plugin(tmp_path):
    data = tmp_path / "data"
    mcp = MCPServer(data, tmp_path)
    try:
        yield mcp, tmp_path, data
    finally:
        mcp.close()
        try:
            info = json.loads((data / "server.json").read_text())
            os.kill(info["pid"], signal.SIGTERM)
            for _ in range(100):
                if not (data / "server.json").exists():
                    break
                time.sleep(0.1)
        except (FileNotFoundError, ProcessLookupError):
            pass


def test_tools_and_instructions(plugin):
    mcp, _, _ = plugin
    init = mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}})["result"]
    assert init["capabilities"] == {"tools": {}}
    # Claude Code keeps the first 4096 characters of a server's instructions.
    assert "tsugumi_open" in init["instructions"] and len(init["instructions"]) <= 4096
    names = [t["name"] for t in mcp.request("tools/list")["result"]["tools"]]
    assert names[0] == "tsugumi_open"
    assert {"project_inspect", "project_save", "run", "run_record", "reference_read"} <= set(names)
    # What belongs to the chat (its attached folders, helpers and the bird's bubble) is not offered.
    assert not {"attached_folder_read", "delegate", "guide_show"} & set(names)
    text, error = mcp.call("project_inspect", {})
    assert error and "tsugumi_open" in text


def test_open_starts_the_host_and_runs_core_tools_seen_by_the_page(plugin):
    mcp, root, data = plugin
    mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}})
    write_project(root / "proj")
    text, error = mcp.call("tsugumi_open", {"folder": "missing"})
    assert error and "create: true" in text
    text, error = mcp.call("tsugumi_open", {"folder": "proj", "browser": False})
    assert not error, text
    assert "Opened" in text and "project_inspect" in text  # the rules come with the first open
    # The host it started is found through server.json, readable by this user alone.
    info_file = data / "server.json"
    assert stat.S_IMODE(info_file.stat().st_mode) == 0o600
    info = json.loads(info_file.read_text())
    page = Events(info["port"], f"{info['cookie']}={info['key']}")
    page.start()
    text, error = mcp.call("project_inspect", {})
    assert not error, text
    snap = json.loads(text)
    assert snap["reply"] == "ProjectSnapshot"
    rev = snap["value"]["rev"]
    project = json.loads(snap["value"]["project_json"])
    project["name"] = "renamed by Claude Code"
    text, error = mcp.call("project_save", {"base_rev": str(rev), "changes": [{"value_type": "SetProject", "value": {"json": json.dumps(project)}}]})
    assert not error, text
    assert json.loads(text)["reply"] == "SaveResult"
    assert json.loads((root / "proj" / "tsugumi.json").read_text())["name"] == "renamed by Claude Code"
    # A second open (another session) reuses the running host; the open page is told to show the project.
    text, error = mcp.call("tsugumi_open", {"folder": str(root / "proj")})
    assert not error and "The open web UI page shows it now" in text and "Rules" not in text
    assert json.loads(info_file.read_text())["pid"] == info["pid"]
    for _ in range(50):
        if any(e.get("type") == "agentOpen" for e in page.events):
            break
        time.sleep(0.1)
    work = [e for e in page.events if e.get("type") == "agentWork"]
    assert [(e["event"]["kind"], e["event"]["name"]) for e in work] == [
        ("toolStarted", "project_inspect"), ("toolFinished", "project_inspect"),
        ("toolStarted", "project_save"), ("toolFinished", "project_save")]
    assert all(e["from"] == "Claude Code" and e["project"] == str((root / "proj").resolve()) for e in work)
    assert any(e.get("type") == "agentOpen" for e in page.events)
    # create: true makes a new project.
    text, error = mcp.call("tsugumi_open", {"folder": "fresh", "create": True, "browser": False})
    assert not error, text
    assert (root / "fresh" / "tsugumi.json").exists()
