"""The web UI's host server: only the page opened through the URL it printed
(its key, then a cookie) reaches /api/, a malformed request fails alone, files
outside the page are not served, and every answer forbids framing and
sniffing. Needs node and a built ui/web/dist (npm run build)."""

from __future__ import annotations

from contextlib import ExitStack, closing, contextmanager
from html.parser import HTMLParser
import http.client
from http.cookies import SimpleCookie
import json
import select
import shutil
import subprocess
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest

from harness.file_events import process_exit, process_file, terminate_process
from conftest import WEB_DIST
from test_claude_code_plugin import isolated_environment, stop_resident_services

ROOT = Path(__file__).resolve().parents[2]
SERVER = WEB_DIST / "server.js"

pytestmark = pytest.mark.skipif(not shutil.which("node") or not SERVER.exists(),
                                reason="needs node and ui/web/dist (cd ui/web && npm run build)")


@contextmanager
def web_server(tmp_path, **environment):
    env = isolated_environment(tmp_path, TSUGUMI_WEB_DATA=str(tmp_path / "data"), **environment)
    p = subprocess.Popen(["node", str(SERVER), "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    try:
        ready = process_file(p, tmp_path / "data/server.json")
        assert ready is not None
        info = json.loads(ready.read_text())
        # The launch URL is the only public source of the page key.
        line = p.stdout.readline()
        launch = urlparse(next(part for part in line.split() if part.startswith("http://")))
        assert launch.hostname == "127.0.0.1" and launch.port == info["port"]
        yield p, info["port"], parse_qs(launch.query)["key"][0]
    finally:
        p.terminate()
        p.wait(10)
        stop_resident_services(tmp_path)


@pytest.fixture
def web(tmp_path):
    with web_server(tmp_path) as server:
        yield server


@pytest.fixture(scope="module")
def http_server(tmp_path_factory):
    with web_server(tmp_path_factory.mktemp("web-http")) as server:
        yield server


def request(port: int, method: str, path: str, headers=None, body=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request(method, path, body=body, headers={"Host": f"127.0.0.1:{port}", **(headers or {})})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r, data


def policy(response):
    directives = [part.split() for part in response.getheader("content-security-policy").split(";")]
    return {parts[0]: set(parts[1:]) for parts in directives if parts}


class Scripts(HTMLParser):
    def __init__(self):
        super().__init__()
        self.sources = []

    def handle_starttag(self, tag, attrs):
        if tag == "script":
            self.sources.append(dict(attrs).get("src"))


def test_only_the_page_with_the_key_reaches_the_api(http_server):
    p, port, key = http_server
    r, _ = request(port, "GET", "/api/env/list")
    assert r.status == 401
    r, _ = request(port, "GET", "/api/events")
    assert r.status == 401
    r, _ = request(port, "GET", "/api/settings")
    assert r.status == 401
    r, _ = request(port, "GET", "/api/env/list", {"Cookie": f"tsugumi_{port}=wrong"})
    assert r.status == 401
    r, _ = request(port, "GET", f"/?key={key}&project=x")
    assert r.status == 302
    location = urlparse(r.getheader("location"))
    assert location.path == "/" and not location.netloc
    query = parse_qs(location.query)
    assert query["project"] == ["x"] and "key" not in query
    cookies = SimpleCookie(r.getheader("set-cookie"))
    session, = cookies.values()
    assert session["httponly"] and session["samesite"].lower() == "strict"
    cookie = f"{session.key}={session.value}"
    r, data = request(port, "GET", "/api/env/list", {"Cookie": cookie})
    assert r.status == 200, data
    json.loads(data)


def test_teardown_stops_host_before_readiness(tmp_path):
    # Keep host.ready absent even after an authenticated request succeeds.
    started = tmp_path / "host.pid"
    binary = tmp_path / "starting-host"
    binary.write_text(f"""#!{sys.executable}
import os
from pathlib import Path
import signal

signals = {{signal.SIGTERM}}
signal.pthread_sigmask(signal.SIG_BLOCK, signals)
started = Path({str(started)!r})
pending = started.with_suffix(".tmp")
pending.write_text(str(os.getpid()))
pending.rename(started)
signal.sigwait(signals)
""")
    binary.chmod(0o700)
    with ExitStack() as cleanup:
        with web_server(tmp_path, TSUGUMI_HOSTD=str(binary)) as (web_process, port, key):
            assert process_file(web_process, started) is not None
            pid = int(started.read_text())
            cleanup.callback(terminate_process, pid)
            exited = cleanup.enter_context(process_exit(pid))
            assert request(port, "GET", "/api/env/list", {"Cookie": page_cookie(port, key)})[0].status == 200
            assert not (tmp_path / "host-core/host.ready").exists()
        assert select.select([exited], [], [], 0)[0], "hostd survived Web fixture teardown"


def test_a_malformed_request_fails_alone_and_nothing_outside_the_page_is_served(http_server):
    p, port, key = http_server
    for bad in ("/%E0", "/api/%E0"):
        r, _ = request(port, "GET", bad)
        assert r.status in (400, 401), bad
    assert p.poll() is None
    r, data = request(port, "GET", "/..%2Fserver.js")
    home, page = request(port, "GET", "/")
    assert home.status == 200
    if r.status != 403:
        assert r.status == 200 and r.getheader("content-type").startswith("text/html")
        fallback, entry = Scripts(), Scripts()
        fallback.feed(data.decode())
        entry.feed(page.decode())
        assert set(filter(None, entry.sources)) <= set(filter(None, fallback.sources))
    csp = policy(home)
    assert csp["frame-ancestors"] == {"'none'"}
    # The page reaches its project's tsugumid over a loopback WebSocket.
    assert "ws://127.0.0.1:*" in csp["connect-src"]
    assert home.getheader("x-content-type-options") == "nosniff"


def test_the_shared_wasm_client_is_served_and_allowed_by_the_page_policy(http_server):
    process, port, _ = http_server
    response, _ = request(port, "GET", "/")
    scripts = policy(response)["script-src"]
    assert "'wasm-unsafe-eval'" in scripts and "'unsafe-eval'" not in scripts
    response, module = request(port, "GET", "/client/tsugumi-client.wasm")
    assert response.status == 200 and response.getheader("content-type") == "application/wasm"
    subprocess.run(["node", "-e", "new WebAssembly.Module(require('node:fs').readFileSync(0))"],
                   input=module, check=True)
    assert process.poll() is None, process.stdout.read()
    response, script = request(port, "GET", "/client/tsugumi-client.js")
    assert response.status == 200 and script
    assert response.getheader("content-type").split(";")[0] in {"text/javascript", "application/javascript"}


def test_the_plot_viewer_is_framed_sandboxed_and_reaches_nothing(http_server):
    """/modules/ (a Card's Plotly figure or Vega-Lite chart) may be framed by the
    page alone, always runs sandboxed (an opaque origin: no cookie, no /api/),
    loads nothing (its script is inline; the page sends it the libraries) and
    connects nowhere; every other answer still forbids framing."""
    p, port, key = http_server
    r, data = request(port, "GET", "/modules/viewer.html")
    assert r.status == 200
    scripts = Scripts()
    scripts.feed(data.decode())
    assert scripts.sources and all(source is None for source in scripts.sources)
    csp = policy(r)
    assert "allow-scripts" in csp["sandbox"] and "allow-same-origin" not in csp["sandbox"]
    assert csp["default-src"] == csp["connect-src"] == {"'none'"}
    assert "'self'" not in csp["script-src"] and csp["frame-ancestors"] == {"'self'"}
    # No nested frame may load a URL: the Card's srcdoc frame cannot navigate itself anywhere.
    assert csp["frame-src"] == {"'none'"}
    assert r.getheader("x-frame-options") == "SAMEORIGIN"
    r, _ = request(port, "GET", "/")
    assert r.getheader("x-frame-options") == "DENY" and policy(r)["frame-ancestors"] == {"'none'"}


def page_cookie(port: int, key: str) -> str:
    r, _ = request(port, "GET", f"/?key={key}")
    return r.getheader("set-cookie").split(";")[0]


def post(port: int, cookie: str, route: str, body: dict):
    r, data = request(port, "POST", route, {"Cookie": cookie, "content-type": "application/json", "x-tsugumi": "1"}, json.dumps(body))
    assert r.status == 200, data
    return json.loads(data)


def test_host_reconnect_preserves_the_pages_showing_a_project(tmp_path):
    from conftest import write_project

    fail = tmp_path / "fail-next-start"
    binary = tmp_path / "host-launcher"
    host = isolated_environment(tmp_path)["TSUGUMI_HOSTD"]
    binary.write_text(f"""#!{sys.executable}
import os
import sys
from pathlib import Path

try:
    Path({str(fail)!r}).unlink()
except FileNotFoundError:
    os.execv({host!r}, [{host!r}, *sys.argv[1:]])
sys.exit(1)
""")
    binary.chmod(0o700)

    with web_server(tmp_path, TSUGUMI_HOSTD=str(binary)) as (_, port, key):
        cookie = page_cookie(port, key)
        project = tmp_path / "proj"
        write_project(project)

        def open_events(connection, page):
            connection.request("GET", f"/api/events?page={page}", headers={"Cookie": cookie})
            response = connection.getresponse()
            assert response.status == 200
            return response

        def wait_event(response, kind):
            for line in response:
                if line.startswith(b"data: "):
                    event = json.loads(line.removeprefix(b"data: "))
                    if event["type"] == kind:
                        return event
                    assert event["type"] != "projectClosed", event
            pytest.fail(f"The browser event stream closed before {kind}")

        with ExitStack() as stack:
            def page_events(page):
                connection = stack.enter_context(closing(http.client.HTTPConnection("127.0.0.1", port, timeout=10)))
                response = open_events(connection, page)
                wait_event(response, "eventsConnected")
                post(port, cookie, "/api/page", {"page": page, "project": str(project)})
                return response

            first = page_events("first")
            second = page_events("second")
            pid = json.loads((tmp_path / "host-core/host.ready").read_text())["pid"]
            fail.touch()
            terminate_process(pid)
            wait_event(first, "hostUnavailable")
            wait_event(second, "hostUnavailable")
            post(port, cookie, "/api/project/close", {"page": "first", "path": str(project)})
            # The status retry reauthenticates and both browser streams survive.
            status, body = request(port, "GET", "/api/chatgpt?attemptId=old-host-attempt", {"Cookie": cookie})
            assert status.status == 200, body
            wait_event(first, "reconnected")
            wait_event(second, "reconnected")
            interrupted = json.loads(body)
            assert interrupted["signingIn"] is False
            assert interrupted["outcome"]["ok"] is False
            post(port, cookie, "/api/project/close", {"page": "second", "path": str(project)})
            assert wait_event(first, "projectClosed")["project"] == str(project.resolve())


def test_core_keeps_running_when_the_last_page_showing_a_project_leaves(web, tmp_path):
    from test_claude_code_plugin import Events
    from conftest import write_project

    _, port, key = web
    cookie = page_cookie(port, key)
    project = tmp_path / "proj"
    write_project(project)
    real = str(project.resolve())
    pages = [Events(port, cookie, "a"), Events(port, cookie, "b"), Events(port, cookie, "child&detached")]
    for p in pages:
        p.start()
    for page in ("a", "b", "child"):
        post(port, cookie, "/api/page", {"page": page, "project": str(project)})
    before = post(port, cookie, "/api/env/start", {"path": str(project)})
    assert before["state"] == "ready", before
    # Another page shows it: it goes on.
    assert post(port, cookie, "/api/project/close", {"page": "a", "path": str(project)})["ok"] is True
    assert not [e for e in pages[2].events if e.get("type") == "projectClosed"]
    # The last page detaches its views while core remains available.
    assert post(port, cookie, "/api/project/close", {"page": "b", "path": str(project)})["ok"] is True
    pages[2].wait(lambda: any(e.get("type") == "projectClosed" for e in pages[2].events))
    assert any(event.get("type") == "projectClosed" and event.get("project") == real
               for event in pages[2].events)
    after = post(port, cookie, "/api/env/start", {"path": str(project)})
    assert (after["endpoint"], after["projectId"], after["token"]) == (before["endpoint"], before["projectId"], before["token"])


def test_environment_stop_is_owned_by_hostd_and_a_later_start_reuses_the_host(web, tmp_path):
    from conftest import write_project

    _, port, key = web
    cookie = page_cookie(port, key)
    project = tmp_path / "restart"
    write_project(project)
    first = post(port, cookie, "/api/env/start", {"path": str(project)})
    assert first["state"] == "ready", first
    agent = json.loads((tmp_path / "host-core/host.ready").read_text())
    assert post(port, cookie, "/api/env/stop", {"path": str(project)})["ok"] is True
    response, data = request(port, "GET", "/api/env/list", {"Cookie": cookie})
    assert response.status == 200
    environment = next(row for row in json.loads(data) if row["project"] == str(project.resolve()))
    assert environment["state"]["state"] == "stopped"
    restarted = post(port, cookie, "/api/env/start", {"path": str(project)})
    assert restarted["state"] == "ready", restarted
    assert restarted["projectId"] == first["projectId"]
    assert restarted["token"] != first["token"]
    assert json.loads((tmp_path / "host-core/host.ready").read_text())["pid"] == agent["pid"]


def test_the_page_is_told_whether_host_folders_can_be_used(http_server):
    _, port, key = http_server
    r, data = request(port, "GET", "/api/status", {"Cookie": page_cookie(port, key)})
    assert r.status == 200
    assert json.loads(data)["hostFolders"] is (sys.platform != "darwin")


def test_a_terminal_is_a_real_terminal(web, tmp_path):
    from test_claude_code_plugin import Events
    from conftest import write_project

    _, port, key = web
    cookie = page_cookie(port, key)
    project = tmp_path / "proj"
    write_project(project)
    page = Events(port, cookie, "a")
    page.start()
    t = post(port, cookie, "/api/terminal/start", {"path": str(project), "cols": 91, "rows": 17})

    def printed() -> str:
        return "".join(e["data"] for e in list(page.events) if e.get("type") == "terminal" and e["id"] == t["id"])

    def until(text: str):
        try:
            page.wait(lambda: text in printed())
        except AssertionError:
            pytest.fail(repr(printed()))

    def send(data: str):
        post(port, cookie, "/api/terminal/input", {"id": t["id"], "data": data})

    # It has a terminal, of the size the page draws, and programs may use it.
    send('''tty; echo "size=$(stty size)" "term=$TERM"; echo 日本語
''')
    until("/dev/")
    until("size=17 91 term=xterm-256color")
    until("日本語")
    post(port, cookie, "/api/terminal/resize", {"id": t["id"], "cols": 100, "rows": 30})
    send('''echo "now=$(stty size)"
''')
    until("now=30 100")
    # A key: ⌃C stops what runs, not the shell.
    send(r'''sh -c 'trap "printf \"\000INTERRUPTED\000\"; exit 0" INT; printf "\000WAITING\000"; read line'
''')
    until('\0WAITING\0')
    send("\x03")
    # Wait for signal handling before sending input that the TTY could flush.
    until('\0INTERRUPTED\0')
    send('''echo after-interrupt-$((1 + 1))
''')
    until("after-interrupt-2")
    post(port, cookie, "/api/terminal/stop", {"id": t["id"]})
    page.wait(lambda: any(e.get("type") == "terminal-exit" and e["id"] == t["id"] for e in page.events))


def test_project_creation_forwards_core_errors_and_accepts_empty_folders(web, tmp_path):
    _, port, key = web
    headers = {"Cookie": f"tsugumi_{port}={key}", "X-Tsugumi": "1", "Content-Type": "application/json"}
    destination = tmp_path / "created"
    destination.mkdir()

    def create(name, flow="leftToRight"):
        response, data = request(port, "POST", "/api/project/create", headers,
                                 json.dumps({"parent": str(tmp_path), "name": name, "flow": flow}))
        return response.status, json.loads(data)

    status, result = create(destination.name)
    assert status == 200 and result["path"] == str(destination.resolve())
    assert json.loads((destination / "layout.json").read_text())["flow"] == "leftToRight"
    status, result = create(destination.name)
    assert status == 400 and result["code"] == "folder_not_empty"
    status, result = create("bad/name")
    assert status == 400 and result["code"] == "invalid_project_name"
    status, result = create("bad-flow", "sideways")
    assert status == 400 and result["code"] == "invalid_flow"
    assert not (tmp_path / "bad-flow").exists()


def test_api_csrf_and_input_validation(http_server):
    _, port, key = http_server
    cookie = page_cookie(port, key)
    # POST without x-tsugumi header is rejected (blocks same-site cross-port CSRF).
    r, _ = request(port, "POST", "/api/page", {"Cookie": cookie, "Content-Type": "application/json"}, "{}")
    assert r.status == 403
    # Malformed JSON returns 400 rather than 500.
    r, _ = request(port, "POST", "/api/page",
                   {"Cookie": cookie, "X-Tsugumi": "1", "Content-Type": "application/json"}, "{not-json")
    assert r.status == 400
