"""The Claude Code plugin's MCP server (web/dist/mcp.js): it starts the web UI
host when none runs (found again through its server.json), opens a project,
and runs the chat Agent's core tools there; a page sees each call (the bird
flies to the Card) and nothing works before a project is open. Needs node, a
built web/dist (npm run build) and tsugumid."""

from __future__ import annotations

import http.client
import json
import os
import re
import shutil
import signal
import stat
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

from conftest import BUILD, TSUGUMID, WEB_DIST, write_project

ROOT = Path(__file__).resolve().parents[2]
MCP = WEB_DIST / "mcp.js"

pytestmark = pytest.mark.skipif(not shutil.which("node") or not MCP.exists() or not TSUGUMID.exists(),
                                reason="needs node, web/dist (cd web && npm run build) and tsugumid")


def isolated_environment(root: Path, **values) -> dict:
    """Keep resident services and credentials inside this test's own folder."""
    env = dict(os.environ)
    for key in ("TSUGUMI_VMHOST", "TSUGUMI_VM_IMAGE", "TSUGUMI_VM_SOURCE"):
        env.pop(key, None)
    env.update(TSUGUMI_AGENT_STATE=str(root / "agent-core"),
               TSUGUMI_SUPPORT_DIR=str(root / "support"),
               TSUGUMI_AGENTD=os.environ.get("TSUGUMI_AGENTD", str(BUILD / "core" / "tsugumi-agentd")),
               TSUGUMI_RPC_CLIENT=os.environ.get("TSUGUMI_RPC_CLIENT", str(BUILD / "client" / "tsugumi-rpc")),
               TSUGUMID=str(TSUGUMID),
               ANTHROPIC_API_KEY="fixture-no-network", OPENAI_API_KEY="fixture-no-network")
    env.update(values)
    return env


def stop_resident_services(root: Path):
    """A UI close intentionally leaves core alive; fixture teardown owns it."""
    for name in ("agent.ready", "server.ready"):
        for ready in root.rglob(name):
            try:
                record = json.loads(ready.read_text())
                os.kill(record["pid"], signal.SIGTERM)
            except (FileNotFoundError, ProcessLookupError, KeyError):
                pass


def page_post(port: int, cookie: str, route: str, body: dict) -> dict:
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request("POST", route, body=json.dumps(body), headers={"Host": f"127.0.0.1:{port}",
              "content-type": "application/json", "x-tsugumi": "1", "Cookie": cookie})
    response = c.getresponse()
    data = json.loads(response.read())
    c.close()
    assert response.status == 200, data
    return data


def allow_pending_access(mcp, name: str, arguments: dict, port: int, cookie: str):
    """The plugin asks; only the authenticated user's page may answer."""
    text, error = mcp.call(name, arguments)
    assert error, text
    request = json.loads(text)
    assert request["code"] == "grant_required", request
    result, denied = mcp.call("agent_access_resolve", {"request_id": request["request_id"], "allow": True})
    assert denied and json.loads(result)["code"] == "permission_denied", result
    page_post(port, cookie, "/api/agent/access/resolve", {"id": request["request_id"], "allow": True})
    return request["request_id"]


class MCPServer:
    def __init__(self, data: Path, cwd: Path):
        env = isolated_environment(cwd, TSUGUMI_WEB_DATA=str(data), CLAUDE_PROJECT_DIR=str(cwd))
        self.p = subprocess.Popen(["node", str(MCP)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
        self.next = 0

    def request(self, method: str, params: dict | None = None) -> dict:
        self.next += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.next, "method": method, "params": params or {}}) + '''
''')
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


def page_cookie(data: Path, port: int) -> str:
    """The page's key, from the URL the host printed when it started (the
    plugin started it: its output is the data folder's server.log)."""
    m = re.findall(rf"http://127\.0\.0\.1:{port}/\?key=([0-9a-f]+)", (data / "server.log").read_text())
    assert m, "the host printed no page URL"
    return f"tsugumi_{port}={m[-1]}"


def status(port: int, method: str, route: str, headers: dict) -> int:
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request(method, route, body=b"{}" if method == "POST" else None,
              headers={"Host": f"127.0.0.1:{port}", "content-type": "application/json", "x-tsugumi": "1", **headers})
    r = c.getresponse()
    r.read()
    c.close()
    return r.status


class Events(threading.Thread):
    """A page: the server's event stream."""

    def __init__(self, port: int, cookie: str, page: str = ""):
        super().__init__(daemon=True)
        self.page, self.port, self.cookie = page, port, cookie
        self.c = http.client.HTTPConnection("127.0.0.1", port, timeout=60)
        self.c.request("GET", f"/api/events?page={page}" if page else "/api/events", headers={"Host": f"127.0.0.1:{port}", "Cookie": cookie})
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

    def shows(self, project: Path | None):
        """Tells the server what this page shows (none: Home), as the page does."""
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        c.request("POST", "/api/page", body=json.dumps({"page": self.page, "project": str(project) if project else None}),
                  headers={"Host": f"127.0.0.1:{self.port}", "content-type": "application/json", "x-tsugumi": "1", "Cookie": self.cookie})
        r = c.getresponse()
        r.read()
        c.close()
        assert r.status == 200

    def told(self) -> list[str]:
        return [e["project"] for e in self.events if e.get("type") == "agentOpen"]


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
        stop_resident_services(tmp_path)


def test_tools_and_instructions(plugin):
    mcp, _, _ = plugin
    init = mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}})["result"]
    assert init["capabilities"] == {"tools": {}}
    # Claude Code keeps the first 4096 characters of a server's instructions.
    assert "tsugumi_open" in init["instructions"] and len(init["instructions"]) <= 4096
    names = [t["name"] for t in mcp.request("tools/list")["result"]["tools"]]
    assert names[0] == "tsugumi_open"
    assert {"project_inspect", "project_save", "run", "run_record", "demo_read", "agent_access", "agent_access_resolve", "agent_conversation_get"} <= set(names)
    # The plugin sees the same core tool catalog and Agent operations.
    assert {"attached_folder_read", "delegate", "guide_show"} <= set(names)
    shared = json.loads((ROOT / "agent/Shared/agent.json").read_text())
    def tool_name(request):
        return re.sub(r"(?<!^)([A-Z])", r"_\1", request.removesuffix("Request")).lower()
    project_names = {tool_name(request) for request in shared["requests"]}
    assert project_names <= set(names)
    assert not {"run_history_delete", "project_cache_clear", "version_checkout", "version_summary",
                "version_init", "version_mark", "view_save"} & set(names)
    text, error = mcp.call("project_inspect", {})
    assert error and "tsugumi_open" in text


def test_concurrent_plugin_and_command_starts_share_one_host(plugin):
    first, root, data = plugin
    write_project(root / "proj")
    second = MCPServer(data, root)
    env = isolated_environment(root, TSUGUMI_WEB_DATA=str(data))
    command = None
    try:
        with ThreadPoolExecutor(max_workers=2) as pool:
            calls = [pool.submit(client.call, "tsugumi_open", {"folder": "proj", "browser": False}) for client in (first, second)]
            command = subprocess.Popen(["node", str(WEB_DIST / "server.js"), "--reuse", "--port", "0"],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
            results = [call.result(timeout=40) for call in calls]
        assert all(not error for _, error in results), results
        info = json.loads((data / "server.json").read_text())
        assert all(f":{info['port']}/" in text for text, _ in results)
        if command.pid != info["pid"]:
            assert command.wait(10) == 0
        else:
            assert command.poll() is None
        # Only a winning detached child publishes a startup URL in server.log.
        log_file = data / "server.log"
        log = log_file.read_text() if log_file.exists() else ""
        assert log.count("Tsugumi web UI:") == (0 if command.pid == info["pid"] else 1)
    finally:
        second.close()
        if command is not None:
            if command.poll() is None:
                command.terminate()
            command.wait(10)


def test_plugin_refuses_a_different_running_build(plugin):
    mcp, root, data = plugin
    write_project(root / "proj")
    text, error = mcp.call("tsugumi_open", {"folder": "proj", "browser": False})
    assert not error, text
    file = data / "server.json"
    original = file.read_text()
    info = json.loads(original)
    info["build"] = "0" * 64
    file.write_text(json.dumps(info))
    try:
        text, error = mcp.call("tsugumi_open", {"folder": "proj", "browser": False})
        assert error and "different Tsugumi build" in text and "Stop that host" in text
        os.kill(info["pid"], 0)
    finally:
        file.write_text(original)


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
    # server.json holds the agent key alone, never the page's (which answers approvals).
    assert set(info) == {"port", "pid", "agentKey", "build"}
    assert re.fullmatch(r"[a-f0-9]{64}", info["build"])
    page_key = page_cookie(data, info["port"])
    assert info["agentKey"] not in page_key
    page = Events(info["port"], page_key)
    page.start()
    allow_pending_access(mcp, "project_inspect", {}, info["port"], page_key)
    text, error = mcp.call("project_inspect", {})
    assert not error, text
    snap = json.loads(text)
    assert snap["reply"] == "ProjectSnapshot"
    rev = snap["value"]["rev"]
    project = json.loads(snap["value"]["project_json"])
    project["name"] = "renamed by Claude Code"
    save = {"base_rev": str(rev), "changes": [{"value_type": "SetProject", "value": {"json": json.dumps(project)}}]}
    allow_pending_access(mcp, "project_save", save, info["port"], page_key)
    text, error = mcp.call("project_save", save)
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
    permissions = [e for e in page.events if e.get("type") == "chat" and e.get("event", {}).get("kind") == "permission"]
    assert len(permissions) >= 2
    assert all(e["project"] == str((root / "proj").resolve()) for e in permissions)
    assert any(e.get("type") == "agentOpen" for e in page.events)
    # create: true makes a new project.
    text, error = mcp.call("tsugumi_open", {"folder": "fresh", "create": True, "browser": False})
    assert not error, text
    assert (root / "fresh" / "tsugumi.json").exists()


def test_open_shows_the_project_without_taking_another_tab_away(plugin):
    """Each browser tab has its own project: Claude Code's goes to a tab that
    has it or one on Home, never over the project of another tab."""
    mcp, root, data = plugin
    mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}})
    for name in ("a", "b", "c"):
        write_project(root / name)
    a, b, c = ((root / n).resolve() for n in ("a", "b", "c"))
    text, error = mcp.call("tsugumi_open", {"folder": "a", "browser": False})
    assert not error, text
    port = json.loads((data / "server.json").read_text())["port"]
    cookie = page_cookie(data, port)
    on_a, home = Events(port, cookie, "page-a"), Events(port, cookie, "page-home")
    on_a.start()
    home.start()
    time.sleep(0.3)
    on_a.shows(a)
    # The project of a tab: it stays where it is, nobody is told to switch.
    text, error = mcp.call("tsugumi_open", {"folder": "a"})
    assert not error and "The open web UI page shows it now" in text
    # Another project: the tab on Home takes it, the other keeps its own.
    text, error = mcp.call("tsugumi_open", {"folder": "b"})
    assert not error and "The open web UI page shows it now" in text
    for _ in range(50):
        if home.told():
            break
        time.sleep(0.1)
    assert home.told() == [str(b)] and on_a.told() == []
    # No tab free: nobody loses a project (the plugin opens a window unless told not to).
    text, error = mcp.call("tsugumi_open", {"folder": "c", "browser": False})
    assert not error and "No web UI page is free" in text
    time.sleep(0.3)
    assert home.told() == [str(b)] and on_a.told() == []
    # A tab back on Home is free again.
    on_a.shows(None)
    text, error = mcp.call("tsugumi_open", {"folder": "c", "browser": False})
    assert not error and "The open web UI page shows it now" in text
    for _ in range(50):
        if on_a.told():
            break
        time.sleep(0.1)
    assert on_a.told() == [str(c)]


def test_the_agent_key_cannot_answer_approvals_or_reach_the_page_routes(plugin):
    """What server.json gives a local client (Claude Code's MCP server) works
    only on the routes the plugin calls: an approval is the user's, on the page."""
    mcp, root, data = plugin
    mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}})
    write_project(root / "proj")
    text, error = mcp.call("tsugumi_open", {"folder": "proj", "browser": False})
    assert not error, text
    info = json.loads((data / "server.json").read_text())
    port, agent = info["port"], {"X-Tsugumi-Agent": info["agentKey"]}
    assert status(port, "POST", "/api/agent/tools", agent) == 200
    for method, route in [("POST", "/api/chat/permission"), ("POST", "/api/page"), ("GET", "/api/events"), ("POST", "/api/terminal/start"),
                          ("GET", "/api/fs"), ("POST", "/api/settings"), ("POST", "/api/chat/send")]:
        assert status(port, method, route, agent) == 401, route
    # A wrong agent key reaches nothing, the page's key everything.
    assert status(port, "POST", "/api/agent/tools", {"X-Tsugumi-Agent": "0" * 64}) == 401
    cookie = page_cookie(data, port)
    for route in ("/api/agent/access", "/api/agent/access/resolve", "/api/agent/access/revoke"):
        assert status(port, "POST", route, agent) == 401, route
    assert status(port, "POST", "/api/agent/access", {"Cookie": cookie}) == 200
    # Real approval is covered by allow_pending_access: it needs this page key.
