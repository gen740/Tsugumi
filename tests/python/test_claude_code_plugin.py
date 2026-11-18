"""The Claude Code plugin's MCP server (ui/web/dist/mcp.js): it starts the web UI
host when none runs (found again through its server.json), opens a project,
and runs the chat Agent's core tools there; a page sees each call (the bird
flies to the Card) and nothing works before a project is open. Needs node, a
built ui/web/dist (npm run build) and tsugumid."""

from __future__ import annotations

import http.client
import json
import os
import queue
import re
import shutil
import shlex
import stat
import subprocess
import threading
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

from harness.file_events import terminate_process, wait_for_file
from conftest import BUILD, TSUGUMID, WEB_DIST, write_project

ROOT = Path(__file__).resolve().parents[2]
MCP = WEB_DIST / "mcp.js"

pytestmark = pytest.mark.skipif(not shutil.which("node") or not MCP.exists() or not TSUGUMID.exists(),
                                reason="needs node, ui/web/dist (cd ui/web && npm run build) and tsugumid")


_socket_directories = {}

def isolated_environment(root: Path, **values) -> dict:
    """Keep resident services and credentials inside this test's own folder."""
    if root not in _socket_directories:
        _socket_directories[root] = tempfile.TemporaryDirectory(
            prefix="tsugumi-", dir=os.environ.get("TSUGUMI_SOCKET_DIR", "/tmp"))
    env = dict(os.environ)
    for key in ("TSUGUMI_VMHOST", "TSUGUMI_VM_IMAGE"):
        env.pop(key, None)
    env.update(TSUGUMI_HOST_STATE=str(root / "host-core"),
               TSUGUMI_SUPPORT_DIR=str(root / "support"),
               TSUGUMI_RUN_SCRATCH_DIR=str(root.resolve() / "run-scratch"),
               TSUGUMI_SOCKET_DIR=_socket_directories[root].name,
               TSUGUMI_HOSTD=os.environ.get("TSUGUMI_HOSTD", str(BUILD / "hostd" / "tsugumi-hostd")),
               TSUGUMI_RPC_CLIENT=os.environ.get("TSUGUMI_RPC_CLIENT", str(BUILD / "client" / "tsugumi-rpc")),
               TSUGUMID=str(TSUGUMID),
               ANTHROPIC_API_KEY="fixture-no-network", OPENAI_API_KEY="fixture-no-network")
    env.update(values)
    # Callers may supply fixture values, never re-enable the real secret store.
    env.update(TSUGUMI_TEST_NO_SECRET_STORE='1',
               TSUGUMI_TEST_RUN_ROOT=os.environ.get('TSUGUMI_TEST_RUN_ROOT', str(root.resolve())))
    home = root / 'home'
    home.mkdir(exist_ok=True)
    env.update(HOME=str(home), CFFIXED_USER_HOME=str(home), USERPROFILE=str(home))
    return env


def test_web_environment_cannot_enable_the_real_secret_store(tmp_path):
    env = isolated_environment(tmp_path, TSUGUMI_TEST_NO_SECRET_STORE="0",
                               TSUGUMI_TEST_RUN_ROOT="/must-not-use-personal-state")
    try:
        assert env["TSUGUMI_TEST_NO_SECRET_STORE"] == "1"
        assert env["TSUGUMI_TEST_RUN_ROOT"] == os.environ["TSUGUMI_TEST_RUN_ROOT"]
        assert Path(env["HOME"]).is_relative_to(tmp_path)
        assert env["HOME"] == env["CFFIXED_USER_HOME"] == env["USERPROFILE"]
    finally:
        _socket_directories.pop(tmp_path).cleanup()


def _stop_matching_processes(argument: str, *, socket_directory: str | None = None):
    listing = subprocess.check_output(["ps", "-axww", "-o", "pid=,stat=,command="], text=True)
    for line in listing.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) != 3:
            continue
        pid, state, command = fields
        if state.startswith("Z") or argument not in command:
            continue
        if socket_directory is not None:
            # MCP's detached Web server has no per-test path in its arguments.
            environment = subprocess.run(["ps", "eww", "-p", pid, "-o", "command="],
                                         capture_output=True, text=True)
            if f"TSUGUMI_SOCKET_DIR={socket_directory}" not in environment.stdout.split():
                continue
        terminate_process(int(pid), grace=None)


def stop_resident_services(root: Path, *, vm_helper: Path | None = None):
    """Stop each launcher before looking for the services it can spawn."""
    directory = _socket_directories[root]
    _stop_matching_processes(f" {WEB_DIST.resolve() / 'server.js'} --reuse --port ",
                             socket_directory=directory.name)
    # hostd creates sockets before it publishes host.ready. Its shutdown
    # stops and waits for the native project daemons it launched.
    _stop_matching_processes(f" --state-dir {root / 'host-core'} --unix {directory.name}/")
    if vm_helper is not None:
        _stop_matching_processes(f" {vm_helper} --support {root / 'support'} --image ")
    directory.cleanup()
    del _socket_directories[root]


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
    def __init__(self, data: Path, cwd: Path, **environment):
        env = isolated_environment(cwd, TSUGUMI_WEB_DATA=str(data), CLAUDE_PROJECT_DIR=str(cwd), **environment)
        self.p = subprocess.Popen(["node", str(MCP)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
        self.next = 0
        self.replies = queue.Queue()
        self.received = {}
        def read():
            for line in self.p.stdout:
                self.replies.put(json.loads(line))
            self.replies.put(None)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def send(self, method: str, params: dict | None = None) -> int:
        self.next += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.next, "method": method, "params": params or {}}) + '''
''')
        self.p.stdin.flush()
        return self.next

    def receive(self, request_id: int) -> dict:
        while request_id not in self.received:
            m = self.replies.get()
            assert m is not None, "the MCP server ended"
            self.received[m["id"]] = m
        return self.received.pop(request_id)

    def notify(self, method: str, params: dict):
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method, "params": params}) + "\n")
        self.p.stdin.flush()

    def request(self, method: str, params: dict | None = None) -> dict:
        return self.receive(self.send(method, params))

    def call(self, name: str, arguments: dict) -> tuple[str, bool]:
        r = self.request("tools/call", {"name": name, "arguments": arguments})["result"]
        return r["content"][0]["text"], bool(r.get("isError"))

    def close(self):
        if not self.p.stdin.closed:
            self.p.stdin.close()
        self.p.wait(10)
        self.reader.join(1)


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
        self.closed = False
        self.changed = threading.Condition()

    def run(self):
        try:
            for raw in self.r:
                line = raw.decode().strip()
                if line.startswith("data:"):
                    with self.changed:
                        self.events.append(json.loads(line[5:]))
                        self.changed.notify_all()
        except (OSError, http.client.HTTPException):
            pass
        finally:
            with self.changed:
                self.closed = True
                self.changed.notify_all()

    def wait(self, predicate):
        with self.changed:
            self.changed.wait_for(lambda: self.closed or predicate())
            assert predicate(), self.events

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


def plugin_at(root, **environment):
    data = root / "data"
    mcp = MCPServer(data, root, **environment)
    try:
        yield mcp, root, data
    finally:
        mcp.close()
        stop_resident_services(root)


@pytest.fixture
def plugin(tmp_path):
    yield from plugin_at(tmp_path)


@pytest.fixture
def waiting_plugin(tmp_path):
    # Keep the production core/host/HTTP/MCP path; only replace Nix evaluation
    # with the same deterministic build gate as the public C++ RPC tests.
    mock = Path(os.environ["TSUGUMI_MOCK_DAEMON"])
    assert mock.is_file()
    control = tmp_path / "mock"
    wrapper = tmp_path / "mock-core"
    wrapper.write_text("#!/bin/sh\nexport TSUGUMI_MOCK_CONTROL=" + shlex.quote(str(control)) +
                       "\nexec " + shlex.quote(str(mock)) + ' "$@"\n')
    wrapper.chmod(0o700)
    write_project(tmp_path / "proj", {"name": "wait", "pkgsPath": "tsugumi:default",
                                      "canvas": {"cards": [{"id": "work"}], "connections": []}})
    definition = tmp_path / "proj/definitions/work"
    definition.mkdir(parents=True)
    for name, text in {"_card.json": '{"language":"bash"}', "default.nix": "{}",
                       "_outputs.fbs": "table Outputs {} root_type Outputs;",
                       "mock-build.json": '{"gate":"web-wait","error":"fixture completion"}'}.items():
        (definition / name).write_text(text)
    yield from plugin_at(tmp_path, TSUGUMID=str(wrapper))


@pytest.mark.parametrize("tool", ["run_status", "run"])
@pytest.mark.parametrize("cancellation", ["notification", "disconnect", "stdio_close"])
def test_external_wait_cancellation_reaches_host_without_stopping_the_run(waiting_plugin, tool, cancellation):
    mcp, root, data = waiting_plugin
    text, error = mcp.call("tsugumi_open", {"folder": "proj", "browser": False})
    assert not error, text
    info = json.loads((data / "server.json").read_text())
    port = info["port"]
    cookie = page_cookie(data, port)
    allow_pending_access(mcp, "project_inspect", {}, port, cookie)
    text, error = mcp.call("project_inspect", {})
    assert not error, text
    revision = json.loads(text)["value"]["rev"]
    start = {"base_rev": str(revision), "card": {"segments": ["work"]}}
    allow_pending_access(mcp, "run", start, port, cookie)
    conversation = page_post(port, cookie, "/api/chat/sessions", {"path": str(root / "proj")})[0]["id"]

    def api(name, arguments):
        return page_post(port, cookie, "/api/agent/call", {"path": str(root / "proj"), "name": name, "arguments": arguments})

    def events():
        return json.loads(api("agent_events", {"conversation_id": conversation})["text"])["events"]

    if tool == "run_status":
        text, error = mcp.call("run", start)
        assert not error, text
        arguments = {"run_id": json.loads(text)["value"]["run"]["run_id"], "wait_seconds": 300}
    else:
        arguments = {**start, "wait_seconds": 300}
    previous = {e["event"]["id"] for e in events() if e["event"]["kind"] == "toolStarted"}
    connection = None
    if cancellation == "disconnect":
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        connection.request("POST", "/api/agent/call", body=json.dumps({"path": str(root / "proj"), "name": tool, "arguments": arguments}),
                           headers={"content-type": "application/json", "x-tsugumi": "1", "x-tsugumi-agent": info["agentKey"]})
    else:
        request_id = mcp.send("tools/call", {"name": tool, "arguments": arguments})
    try:
        wait_for_file(root / "mock/web-wait.started")
        # The external Agent tool history can only be read through agent_events.
        while True:
            started = [e["event"]["id"] for e in events() if e["event"]["kind"] == "toolStarted" and e["event"]["id"] not in previous]
            if started:
                break
            time.sleep(0.01)
        assert len(started) == 1
        # A second request on the shared host connection still works.
        assert not api("project_inspect", {})["isError"]
        if cancellation == "notification":
            mcp.notify("notifications/cancelled", {"requestId": request_id, "reason": "test Stop"})
            assert mcp.receive(request_id)["result"]["isError"]
        elif cancellation == "stdio_close":
            mcp.close()
        else:
            connection.close()
        # No push event is available for an external call ending after disconnect.
        while True:
            finished = [e["event"] for e in events() if e["event"]["kind"] == "toolFinished" and e["event"]["id"] == started[0]]
            if finished:
                break
            time.sleep(0.01)
        assert not finished[0]["ok"], finished
        current = api("run_status", {})
        assert not current["isError"]
        assert json.loads(current["text"])["value"]["state"] == "preparing"
        assert not api("project_inspect", {})["isError"]
    finally:
        (root / "mock/web-wait.release").write_text("release")
        if connection:
            connection.close()


def test_tools_and_instructions(plugin):
    mcp, root, _ = plugin
    init = mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}})["result"]
    assert isinstance(init["capabilities"]["tools"], dict)
    # Claude Code keeps the first 4096 characters of a server's instructions.
    assert len(init["instructions"]) <= 4096
    tools = mcp.request("tools/list")["result"]["tools"]
    names = [t["name"] for t in tools]
    assert "tsugumi_open" in names
    assert len(names) == len(set(names))
    assert {"project_inspect", "project_save", "run", "run_record", "demo_read", "agent_access", "agent_access_resolve", "agent_conversation_get"} <= set(names)
    # The plugin sees the same core tool catalog and Agent operations.
    assert {"tools", "delegate", "demo_list", "project_edit"} <= set(names)
    assert "attached_folder_read" not in names
    assert not any(name.startswith(("guide_", "agent_guide_")) for name in names)
    shared = json.loads((ROOT / "hostd/agent/Shared/agent.json").read_text())
    project_names = set(shared["modelTools"]["core"])
    assert project_names <= set(names)
    assert not {"view_save", "session_hello", "project_open", "agent_hello", "agent_tool_call_cancel"} & set(names)
    text, error = mcp.call("project_inspect", {})
    assert error
    assert not (root / "proj").exists()
    write_project(root / "proj")
    text, error = mcp.call("tsugumi_open", {"folder": str(root / "proj"), "browser": False})
    assert not error, text
    text, error = mcp.call("tools", {"family": "files"})
    assert not error, text
    assert {tool["action"] for tool in json.loads(text)} == set(shared["modelTools"]["families"]["files"])


def test_demo_list_is_compact_and_demo_read_uses_repository_paths(plugin):
    mcp, root, _ = plugin
    write_project(root / "proj")
    text, error = mcp.call("tsugumi_open", {"folder": str(root / "proj"), "browser": False})
    assert not error, text
    text, error = mcp.call("demo_list", {})
    assert not error, text
    demos = json.loads(text)
    report = next(demo for demo in demos if demo['path'] == 'en/report')
    assert report['name'] and report['description']
    for path in ("en/index.json", "en/report/definitions/generate/src/main.py"):
        text, error = mcp.call("demo_read", {"path": path})
        assert not error, text
        expected = (ROOT / "demos" / path).read_text()
        if path.endswith(".json"):
            assert json.loads(text) == json.loads(expected)
        else:
            assert text == expected


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
        assert error and text
        assert json.loads(file.read_text()) == info
        os.kill(info["pid"], 0)
    finally:
        file.write_text(original)


def test_open_starts_the_host_and_runs_core_tools_seen_by_the_page(plugin):
    mcp, root, data = plugin
    mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}})
    write_project(root / "proj")
    text, error = mcp.call("tsugumi_open", {"folder": "missing"})
    assert error
    assert not (root / "missing").exists()
    text, error = mcp.call("tsugumi_open", {"folder": "proj", "browser": False})
    assert not error, text
    _, _, instructions = text.partition("\n\n")
    assert {"project_inspect", "project_edit"} <= set(re.findall(r"[a-z_]+", instructions))
    # The host it started is found through server.json, readable by this user alone.
    info_file = data / "server.json"
    assert stat.S_IMODE(info_file.stat().st_mode) == 0o600
    info = json.loads(info_file.read_text())
    # server.json holds the agent key alone, never the page's (which answers approvals).
    assert {"port", "pid", "agentKey", "build"} <= info.keys()
    assert not {"key", "userKey", "pageKey", "user_token"} & info.keys()
    assert re.fullmatch(r"[a-f0-9]{64}", info["build"])
    page_key = page_cookie(data, info["port"])
    assert info["agentKey"] not in page_key
    page = Events(info["port"], page_key)
    page.start()
    read_request = allow_pending_access(mcp, "project_inspect", {}, info["port"], page_key)
    text, error = mcp.call("project_inspect", {})
    assert not error, text
    snap = json.loads(text)
    assert snap["reply"] == "ProjectSnapshot"
    rev = snap["value"]["rev"]
    project = json.loads(snap["value"]["project_json"])
    project["name"] = "renamed by Claude Code"
    save = {"base_rev": str(rev), "changes": [{"value_type": "SetProject", "value": {"json": json.dumps(project)}}]}
    save_request = allow_pending_access(mcp, "project_save", save, info["port"], page_key)
    text, error = mcp.call("project_save", save)
    assert not error, text
    assert json.loads(text)["reply"] == "SaveResult"
    assert json.loads((root / "proj" / "tsugumi.json").read_text())["name"] == "renamed by Claude Code"
    # A second open (another session) reuses the running host; the open page is told to show the project.
    text, error = mcp.call("tsugumi_open", {"folder": str(root / "proj"), "browser": False})
    assert not error and instructions not in text
    assert json.loads(info_file.read_text())["pid"] == info["pid"]
    page.wait(lambda: any(e.get("type") == "agentOpen" for e in page.events))
    permissions = [e for e in page.events if e.get("type") == "chat" and e.get("event", {}).get("kind") == "permission"]
    assert {e["event"]["id"] for e in permissions if e["project"] == str((root / "proj").resolve())} >= {
        read_request, save_request
    }
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
    on_a.shows(a)
    # The project of a tab: it stays where it is, nobody is told to switch.
    text, error = mcp.call("tsugumi_open", {"folder": "a", "browser": False})
    assert not error, text
    # Another project: the tab on Home takes it, the other keeps its own.
    text, error = mcp.call("tsugumi_open", {"folder": "b", "browser": False})
    assert not error, text
    home.wait(lambda: bool(home.told()))
    assert home.told() == [str(b)] and on_a.told() == []
    # No tab free: nobody loses a project (the plugin opens a window unless told not to).
    text, error = mcp.call("tsugumi_open", {"folder": "c", "browser": False})
    assert not error, text
    assert home.told() == [str(b)] and on_a.told() == []
    # A tab back on Home is free again.
    on_a.shows(None)
    text, error = mcp.call("tsugumi_open", {"folder": "c", "browser": False})
    assert not error, text
    on_a.wait(lambda: bool(on_a.told()))
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
