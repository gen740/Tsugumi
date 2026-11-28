"""The Claude Code plugin's MCP server (`tsugumi mcp`): it starts the host
when none runs (the same `tsugumi-hostd --ensure` as `tsugumi web`), shows a
project through hostd's ShowRequest as the external principal, and runs the
chat Agent's core tools there; a page is told what to show, and only the
user answers access requests. Needs build/cli/tsugumi, hostd and tsugumid."""

from __future__ import annotations

import json
import os
import queue
import re
import shlex
import socket
import struct
import subprocess
import threading
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

from harness.file_events import terminate_process, wait_for_file
from conftest import BUILD, CLI, HOSTD, TSUGUMID, WEB_ROOT, write_project, write_card

from harness.demos import demos_dir

ROOT = Path(__file__).resolve().parents[2]

pytestmark = pytest.mark.skipif(not CLI.exists() or not HOSTD.exists() or not TSUGUMID.exists(),
                                reason="needs build/cli/tsugumi, tsugumi-hostd and tsugumid")


_socket_directories = {}

def isolated_environment(root: Path, **values) -> dict:
    """Keep resident services and credentials inside this test's own folder."""
    if root not in _socket_directories:
        _socket_directories[root] = tempfile.TemporaryDirectory(
            prefix="tsugumi-", dir=os.environ.get("TSUGUMI_SOCKET_DIR", "/tmp"))
    env = dict(os.environ)
    for key in ("TSUGUMI_VMHOST", "TSUGUMI_VM_IMAGE", "ANTHROPIC_API_KEY", "OPENAI_API_KEY"):
        env.pop(key, None)
    env.update(TSUGUMI_HOST_STATE=str(root / "host-core"),
               TSUGUMI_SUPPORT_DIR=str(root / "support"),
               TSUGUMI_WEB_DATA=str(root / "data"),
               TSUGUMI_WEB_ROOT=str(WEB_ROOT),
               TSUGUMI_RUN_SCRATCH_DIR=str(root.resolve() / "run-scratch"),
               TSUGUMI_SOCKET_DIR=_socket_directories[root].name,
               TSUGUMI_HOSTD=str(HOSTD),
               TSUGUMI_REPO=str(ROOT),
               TSUGUMID=str(TSUGUMID))
    env.update(values)
    env.update(TSUGUMI_TEST_RUN_ROOT=os.environ.get('TSUGUMI_TEST_RUN_ROOT', str(root.resolve())))
    home = root / 'home'
    home.mkdir(exist_ok=True)
    env.update(HOME=str(home), CFFIXED_USER_HOME=str(home), USERPROFILE=str(home))
    return env


def _stop_matching_processes(*arguments: str, socket_directory: str | None = None):
    listing = subprocess.check_output(["ps", "-axww", "-o", "pid=,stat=,command="], text=True)
    for line in listing.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) != 3:
            continue
        pid, state, command = fields
        if state.startswith("Z") or not all(argument in command for argument in arguments):
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
    # hostd creates sockets before it publishes host.ready. Its shutdown
    # stops and waits for the native project daemons it launched.
    _stop_matching_processes(f" --state-dir {root / 'host-core'} ",
                             f" --unix {directory.name}/")
    if vm_helper is not None:
        _stop_matching_processes(f" {vm_helper} --support {root / 'support'} --image ")
    directory.cleanup()
    del _socket_directories[root]


class UserAgent:
    """The user's Agent socket, from host.ready (what the page reaches over
    its cookie): it answers access requests, which the plugin cannot."""

    def __init__(self, root: Path):
        self.ready = json.loads((root / "host-core/host.ready").read_text())
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(60)
        self.socket.connect(self.ready["unix_socket"])
        self.request_id = 0
        self.call("HelloRequest", token=self.ready["user_token"])

    def call(self, name, /, **fields):
        """One request as JSON through flatc (the Agent schema's own text form)."""
        self.request_id += 1
        work = Path(tempfile.mkdtemp(prefix="agent-", dir=os.environ.get("TSUGUMI_SOCKET_DIR", "/tmp")))
        try:
            flatc = os.environ.get("TSUGUMI_FLATC", "flatc")
            schema = str(ROOT / "protocol/agent.fbs")
            message = work / "request.json"
            message.write_text(json.dumps({"request_id": self.request_id, "payload_type": "Request",
                                           "payload": {"value_type": name, "value": fields}}))
            subprocess.run([flatc, "-b", "-o", str(work), schema, str(message)], check=True)
            data = message.with_suffix(".bin").read_bytes()
            self.socket.sendall(struct.pack("<I", len(data)) + data)
            while True:
                size, = struct.unpack("<I", self._exact(4))
                reply = work / "reply.bin"
                reply.write_bytes(self._exact(size))
                subprocess.run([flatc, "-t", "--strict-json", "--defaults-json", "-o", str(work), schema, "--", str(reply)], check=True)
                result = json.loads(reply.with_suffix(".json").read_text())
                if result.get("payload_type") == "Reply" and result.get("request_id") == self.request_id:
                    break
        finally:
            subprocess.run(["rm", "-rf", str(work)])
        value = result["payload"]
        if value["value_type"] == "Failure":
            raise RuntimeError(f"{value['value']['code']}: {value['value']['message']}")
        return value.get("value", {})

    def _exact(self, size):
        data = b""
        while len(data) < size:
            chunk = self.socket.recv(size - len(data))
            assert chunk, "hostd closed the Agent socket"
            data += chunk
        return data

    def close(self):
        self.socket.close()


def allow_pending_access(mcp, name: str, arguments: dict, user: UserAgent):
    """The plugin asks; only the authenticated user may answer."""
    text, error = mcp.call(name, arguments)
    assert error, text
    request = json.loads(text)
    assert request["code"] == "grant_required", request
    result, denied = mcp.call("agent_access_resolve", {"request_id": request["request_id"], "allow": True})
    assert denied and "authenticated user" in result, result
    user.call("AccessResolveRequest", request_id=request["request_id"], allow=True)
    return request["request_id"]


class MCPServer:
    def __init__(self, cwd: Path, **environment):
        env = isolated_environment(cwd, CLAUDE_PROJECT_DIR=str(cwd), **environment)
        self.p = subprocess.Popen([str(CLI), "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, env=env)
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
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.next, "method": method, "params": params or {}}) + "\n")
        self.p.stdin.flush()
        return self.next

    def receive(self, request_id: int) -> dict:
        while request_id not in self.received:
            m = self.replies.get(timeout=300)
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


def web(root: Path, *arguments) -> str:
    """`tsugumi web --no-browser`: the URL with the page key (the user's)."""
    r = subprocess.run([str(CLI), "web", "--no-browser", *map(str, arguments)], capture_output=True, text=True,
                       env=isolated_environment(root), timeout=300)
    assert r.returncode == 0, r.stderr
    return r.stdout


def page(root: Path):
    """A Web page attached to the host: a Host WebSocket with the page cookie."""
    from test_web_server import page_cookie, page_host
    url = re.search(r"http://127\.0\.0\.1:(\d+)/\?key=([0-9a-f]+)", web(root))
    port, key = int(url.group(1)), url.group(2)
    return page_host(port, page_cookie(port, key))


def told(host, kind: str = "PageShow") -> str:
    from test_web_server import next_event
    return next_event(host, kind).project.decode()


def plugin_at(root, **environment):
    mcp = MCPServer(root, **environment)
    try:
        yield mcp, root
    finally:
        mcp.close()
        stop_resident_services(root)


@pytest.fixture
def plugin(tmp_path):
    yield from plugin_at(tmp_path)


@pytest.fixture
def waiting_plugin(tmp_path):
    # Keep the production core/host/MCP path; only replace Nix evaluation
    # with the same deterministic build gate as the public C++ RPC tests.
    mock = Path(os.environ["TSUGUMI_MOCK_DAEMON"])
    assert mock.is_file()
    control = tmp_path / "mock"
    wrapper = tmp_path / "mock-core"
    wrapper.write_text("#!/bin/sh\nexport TSUGUMI_MOCK_CONTROL=" + shlex.quote(str(control)) +
                       "\nexec " + shlex.quote(str(mock)) + ' "$@"\n')
    wrapper.chmod(0o700)
    write_project(tmp_path / "proj", {"name": "wait", "nixPath": [],
                                      "canvas": {"cards": [{"id": "work"}], "connections": []}})
    definition = tmp_path / "proj/definitions/work"
    definition.mkdir(parents=True)
    write_card(definition)
    for name, text in {"default.nix": "{}",
                       "_outputs.fbs": "table Outputs {} root_type Outputs;",
                       "mock-build.json": '{"gate":"web-wait","error":"fixture completion"}'}.items():
        (definition / name).write_text(text)
    yield from plugin_at(tmp_path, TSUGUMID=str(wrapper))


def external_conversation(user: UserAgent) -> str:
    listed = user.call("ConversationListRequest")
    ids = [c["id"] for c in listed.get("conversations", []) if c["id"].startswith("external-")]
    assert len(ids) == 1, ids
    return ids[0]


@pytest.mark.parametrize(("tool", "cancellation"), [
    ("run_status", "notification"), ("run", "kill"), ("run_status", "stdio_close"),
])
def test_external_wait_cancellation_reaches_host_without_stopping_the_run(waiting_plugin, tool, cancellation):
    mcp, root = waiting_plugin
    text, error = mcp.call("tsugumi_open", {"folder": "proj", "browser": False})
    assert not error, text
    user = UserAgent(root)
    allow_pending_access(mcp, "project_inspect", {}, user)
    text, error = mcp.call("project_inspect", {})
    assert not error, text
    revision = json.loads(text)["value"]["rev"]
    start = {"base_rev": str(revision), "card": {"segments": ["work"]}}
    allow_pending_access(mcp, "run", start, user)
    conversation = external_conversation(user)

    def events():
        page = user.call("EventsRequest", conversation_id=conversation, limit=1000)
        return [e["event"] for e in page.get("events", [])]

    if tool == "run_status":
        text, error = mcp.call("run", start)
        assert not error, text
        arguments = {"run_id": json.loads(text)["value"]["run"]["run_id"], "wait_seconds": 300}
    else:
        arguments = {**start, "wait_seconds": 300}
    previous = {e.get("id") for e in events() if e["kind"] == "toolStarted"}
    waiting = MCPServer(root) if cancellation == "kill" else mcp
    if waiting is not mcp:
        assert not waiting.call("tsugumi_open", {"folder": "proj", "browser": False})[1]
    request_id = waiting.send("tools/call", {"name": tool, "arguments": arguments})
    try:
        wait_for_file(root / "mock/web-wait.started")
        while True:
            started = [e.get("id") for e in events() if e["kind"] == "toolStarted" and e.get("id") not in previous]
            if started:
                break
            time.sleep(0.01)
        assert len(started) == 1
        if cancellation == "notification":
            mcp.notify("notifications/cancelled", {"requestId": request_id, "reason": "test Stop"})
            assert mcp.receive(request_id)["result"]["isError"]
        elif cancellation == "stdio_close":
            mcp.close()
        else:
            waiting.p.kill()
            waiting.p.wait(10)
        # No push event is available for an external call ending after disconnect.
        while True:
            finished = [e for e in events() if e["kind"] == "toolFinished" and e.get("id") == started[0]]
            if finished:
                break
            time.sleep(0.01)
        assert not finished[0]["ok"]
    finally:
        (root / "mock/web-wait.release").write_text("release")
        user.close()


def test_tools_instructions_and_demo_catalog(plugin):
    mcp, root = plugin
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
    assert not any(name.startswith(("guide_", "agent_guide_")) for name in names)
    shared = json.loads((ROOT / "hostd/agent/Shared/agent.json").read_text())
    assert set(shared["modelTools"]["core"]) <= set(names)
    assert not {"agent_hello", "agent_tool_call_cancel", "agent_call_id", "agent_show"} & set(names)
    text, error = mcp.call("project_inspect", {})
    assert error
    write_project(root / "proj")
    text, error = mcp.call("tsugumi_open", {"folder": str(root / "proj"), "browser": False})
    assert not error, text
    text, error = mcp.call("tools", {"family": "files"})
    assert not error, text
    assert {tool["action"] for tool in json.loads(text)} == set(shared["modelTools"]["families"]["files"])
    text, error = mcp.call("demo_list", {})
    assert not error, text
    report = next(demo for demo in json.loads(text) if demo['path'] == 'en/report')
    assert report['name'] and report['description']
    for path in ("en/index.json", "en/report/definitions/generate/src/main.py"):
        text, error = mcp.call("demo_read", {"path": path})
        assert not error, text
        expected = (demos_dir() / path).read_text()
        assert (json.loads(text) == json.loads(expected)) if path.endswith(".json") else text == expected
    # Malformed requests are JSON-RPC errors, and the server goes on.
    assert mcp.request("initialize", [1])["error"]["code"] == -32602
    assert mcp.request("ping")["result"] == {}


def test_concurrent_plugin_and_command_starts_share_one_host(plugin):
    first, root = plugin
    write_project(root / "proj")
    second = MCPServer(root)
    try:
        with ThreadPoolExecutor(max_workers=3) as pool:
            calls = [pool.submit(client.call, "tsugumi_open", {"folder": "proj", "browser": False}) for client in (first, second)]
            command = pool.submit(web, root)
            results = [call.result(timeout=300) for call in calls]
            printed = command.result(timeout=300)
        assert all(not error for _, error in results), results
        port = re.search(r"http://127\.0\.0\.1:(\d+)/", printed).group(1)
        assert all(f":{port}/" in text for text, _ in results)
        hosts = subprocess.check_output(["ps", "-axww", "-o", "command="], text=True)
        assert hosts.count(f"--state-dir {root / 'host-core'} ") == 1
    finally:
        second.close()


def test_open_runs_core_tools_and_only_the_user_answers_access(plugin):
    mcp, root = plugin
    mcp.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}})
    write_project(root / "proj")
    template = root / "proj/templates/entry/start"
    template.mkdir(parents=True)
    trigger = write_card(template, "trigger")
    text, error = mcp.call("tsugumi_open", {"folder": "missing"})
    assert error and "create: true" in text
    assert not (root / "missing").exists()
    text, error = mcp.call("tsugumi_open", {"folder": "proj", "browser": False})
    assert not error, text
    _, _, instructions = text.partition("\n\n")
    assert {"project_inspect", "project_edit"} <= set(re.findall(r"[a-z_]+", instructions))
    # The external principal gets the Web UI's origin, never the page key.
    assert "key=" not in text
    user = UserAgent(root)
    try:
        allow_pending_access(mcp, "project_inspect", {}, user)
        text, error = mcp.call("project_inspect", {})
        assert not error, text
        snap = json.loads(text)
        project = json.loads(snap["value"]["project_json"])
        project["name"] = "renamed by Claude Code"
        save = {"base_rev": str(snap["value"]["rev"]), "changes": [{"value_type": "SetProject", "value": {"json": json.dumps(project)}}]}
        allow_pending_access(mcp, "project_save", save, user)
        text, error = mcp.call("project_save", save)
        assert not error, text
        assert json.loads((root / "proj" / "tsugumi.json").read_text())["name"] == "renamed by Claude Code"

        text, error = mcp.call("template_list", {})
        assert not error, text
        assert json.loads(text)["value"]["entries"] == [{"name": "entry", "store": "project"}]
        text, error = mcp.call("tools", {
            "family": "templates", "action": "copy", "arguments": {"store": "project", "name": "entry"},
        })
        assert not error, text
        assert json.loads(text)["reply"] == "SaveResult"
        assert json.loads((root / "proj/definitions/start/_card.json").read_text()) == trigger

        source = root / "local-template"
        (source / "start").mkdir(parents=True)
        code = write_card(source / "start", grants={"network": True, "gui": False})
        add = {"family": "templates", "action": "add", "arguments": {
            "store": "project", "name": "added-via-mcp", "path": str(source),
        }}
        added = root / "proj/templates/added-via-mcp/start/_card.json"
        allow_pending_access(mcp, "tools", add, user)
        assert not added.exists()
        text, error = mcp.call("tools", add)
        assert not error, text
        assert json.loads(text)["reply"] == "SaveResult"
        assert added.is_file()

        guarded_copy = {"family": "templates", "action": "copy", "arguments": {
            "store": "project", "name": "added-via-mcp",
        }}
        copied = root / "proj/definitions/start_2/_card.json"
        allow_pending_access(mcp, "tools", guarded_copy, user)
        assert not copied.exists()
        text, error = mcp.call("tools", guarded_copy)
        assert not error, text
        assert json.loads(text)["reply"] == "SaveResult"
        assert json.loads(copied.read_text()) == code
    finally:
        user.close()
    # A second open reuses the host and repeats no rules; create: true makes a new project.
    text, error = mcp.call("tsugumi_open", {"folder": str(root / "proj"), "browser": False})
    assert not error and instructions not in text
    text, error = mcp.call("tsugumi_open", {"folder": "fresh", "create": True, "browser": False})
    assert not error, text
    assert (root / "fresh" / "tsugumi.json").exists()


def test_open_shows_the_project_without_taking_another_tab_away(plugin):
    """Each page has its own project: Claude Code's goes to a page that has it
    or one on Home, never over the project of another page."""
    from contextlib import closing
    mcp, root = plugin
    for name in ("a", "b", "c"):
        write_project(root / name)
    a, b, c = (str((root / n).resolve()) for n in ("a", "b", "c"))
    text, error = mcp.call("tsugumi_open", {"folder": "a", "browser": False})
    assert not error, text
    with closing(page(root)) as on_a, closing(page(root)) as home:
        on_a.call("HostPageRequest", project=a)
        home.call("HostPageRequest")
        # The project of a page: it stays where it is, and that page comes forward.
        text, error = mcp.call("tsugumi_open", {"folder": "a", "browser": False})
        assert not error and "page shows it now" in text, text
        assert told(on_a) == a
        # Another project: the page on Home takes it.
        text, error = mcp.call("tsugumi_open", {"folder": "b", "browser": False})
        assert not error and "page shows it now" in text, text
        assert told(home) == b
        # No page free: nobody loses a project.
        text, error = mcp.call("tsugumi_open", {"folder": "c", "browser": False})
        assert not error and "No Tsugumi window or page is free" in text, text
        # A page back on Home is free again.
        on_a.call("HostPageRequest")
        text, error = mcp.call("tsugumi_open", {"folder": "c", "browser": False})
        assert not error, text
        assert told(on_a) == c


def test_tool_subcommands_work_as_the_external_principal(tmp_path):
    """`tsugumi <tool>` is the MCP server's catalog on the command line: the
    same tools, typed options from their schemas, JSON out, and the same
    access as Claude Code's (only the user answers access requests)."""
    write_project(tmp_path / "proj")
    env = isolated_environment(tmp_path)

    def tsugumi(*arguments, cwd=tmp_path / "proj"):
        return subprocess.run([str(CLI), *arguments], capture_output=True, text=True, env=env, cwd=cwd, timeout=300)

    try:
        listed = tsugumi("--help")
        assert listed.returncode == 0, listed.stderr
        assert "  run plan - " in listed.stdout and "  runs <action>" in listed.stdout
        assert "  agent access resolve - " in listed.stdout
        plan = tsugumi("run", "plan", "--help")
        assert plan.returncode == 0 and "--card <object> (required)" in plan.stdout
        assert tsugumi("project", "inspect", "--bogus", "1").returncode == 1
        asked = tsugumi("project", "inspect")
        assert asked.returncode == 1, asked.stderr
        request = json.loads(asked.stdout)
        assert request["code"] == "grant_required"
        denied = tsugumi("agent", "access", "resolve", "--request-id", request["request_id"], "--allow")
        assert denied.returncode == 1 and "authenticated user" in denied.stderr
        raw = tsugumi("call", "AccessResolveRequest", json.dumps({"request_id": request["request_id"], "allow": True}))
        assert raw.returncode == 1 and json.loads(raw.stdout)["code"] == "permission_denied"
        user = UserAgent(tmp_path)
        try:
            user.call("AccessResolveRequest", request_id=request["request_id"], allow=True)
        finally:
            user.close()
        inspected = tsugumi("project", "inspect")
        assert inspected.returncode == 0, inspected.stdout
        assert json.loads(inspected.stdout)["reply"] == "ProjectSnapshot"
        # A family action: `tsugumi runs status`, typed from the project's `tools` listing.
        status = tsugumi("runs", "status")
        assert status.returncode == 0, status.stdout
        assert json.loads(status.stdout)["reply"] == "RunStatus"
        actions = tsugumi("runs", "--help")
        assert actions.returncode == 0 and "  runs status - " in actions.stdout
    finally:
        stop_resident_services(tmp_path)
