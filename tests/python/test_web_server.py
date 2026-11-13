"""The web UI's host server: only the page opened through the URL it printed
(its key, then a cookie) reaches /api/, a malformed request fails alone, files
outside the page are not served, and every answer forbids framing and
sniffing. Needs node and a built web/dist (npm run build)."""

from __future__ import annotations

import base64
import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest

from conftest import WEB_DIST
from test_claude_code_plugin import isolated_environment, stop_resident_services

ROOT = Path(__file__).resolve().parents[2]
SERVER = WEB_DIST / "server.js"

pytestmark = pytest.mark.skipif(not shutil.which("node") or not SERVER.exists(),
                                reason="needs node and web/dist (cd web && npm run build)")


@pytest.fixture
def web(tmp_path):
    env = isolated_environment(tmp_path, TSUGUMI_WEB_DATA=str(tmp_path / "data"))
    p = subprocess.Popen(["node", str(SERVER), "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    try:
        line = p.stdout.readline()
        m = re.search(r"http://127\.0\.0\.1:(\d+)/\?key=([0-9a-f]+)", line)
        assert m, line
        yield p, int(m.group(1)), m.group(2)
    finally:
        p.terminate()
        p.wait(10)
        stop_resident_services(tmp_path)


def request(port: int, method: str, path: str, headers=None, body=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request(method, path, body=body, headers={"Host": f"127.0.0.1:{port}", **(headers or {})})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r, data


def test_only_the_page_with_the_key_reaches_the_api(web):
    p, port, key = web
    r, _ = request(port, "GET", "/api/env/list")
    assert r.status == 401
    r, _ = request(port, "GET", "/api/events")
    assert r.status == 401
    r, _ = request(port, "GET", "/api/settings")
    assert r.status == 401
    r, _ = request(port, "GET", "/api/env/list", {"Cookie": f"tsugumi_{port}=wrong"})
    assert r.status == 401
    r, _ = request(port, "GET", f"/?key={key}&project=x")
    assert r.status == 302 and r.getheader("location") == "/?project=x"
    cookie = r.getheader("set-cookie").split(";")[0]
    assert "HttpOnly" in r.getheader("set-cookie") and "SameSite=Strict" in r.getheader("set-cookie")
    r, data = request(port, "GET", "/api/env/list", {"Cookie": cookie})
    assert r.status == 200, data
    json.loads(data)
    # Changes also need the header a cross-origin page cannot send.
    r, _ = request(port, "POST", "/api/terminal/start", {"Cookie": cookie, "content-type": "application/json"}, b"{}")
    assert r.status == 403


def test_a_malformed_request_fails_alone_and_nothing_outside_the_page_is_served(web):
    p, port, key = web
    for bad in ("/%E0", "/api/%E0"):
        r, _ = request(port, "GET", bad)
        assert r.status in (400, 401), bad
    assert p.poll() is None
    r, data = request(port, "GET", "/..%2Fserver.js")
    assert r.status == 403 or b"<!doctype" in data.lower()
    assert b"chatRoutes" not in data
    r, _ = request(port, "GET", "/")
    assert r.status == 200
    assert "frame-ancestors 'none'" in r.getheader("content-security-policy")
    # The page reaches its project's tsugumid over a loopback WebSocket.
    assert "ws://127.0.0.1:*" in r.getheader("content-security-policy")
    assert r.getheader("x-content-type-options") == "nosniff"


def test_the_shared_wasm_client_is_served_and_allowed_by_the_page_policy(web):
    _, port, _ = web
    response, _ = request(port, "GET", "/")
    policy = response.getheader("content-security-policy")
    assert "'wasm-unsafe-eval'" in policy and "'unsafe-eval'" not in policy
    response, module = request(port, "GET", "/client/tsugumi-client.wasm")
    assert response.status == 200 and response.getheader("content-type") == "application/wasm"
    assert module[:4] == b"\x00asm"
    response, script = request(port, "GET", "/client/tsugumi-client.js")
    assert response.status == 200 and b"<!doctype" not in script.lower()


def test_the_plot_viewer_is_framed_sandboxed_and_reaches_nothing(web):
    """/modules/ (a Card's Plotly figure or Vega-Lite chart) may be framed by the
    page alone, always runs sandboxed (an opaque origin: no cookie, no /api/),
    loads nothing (its script is inline; the page sends it the libraries) and
    connects nowhere; every other answer still forbids framing."""
    p, port, key = web
    r, data = request(port, "GET", "/modules/viewer.html")
    assert r.status == 200 and b"tsugumiRender" in data and b'src="viewer.js"' not in data
    csp = r.getheader("content-security-policy")
    directives = {d.strip().split(" ", 1)[0]: d.strip() for d in csp.split(";")}
    assert directives["sandbox"] == "sandbox allow-scripts"  # no allow-same-origin
    assert directives["default-src"] == "default-src 'none'" and directives["connect-src"] == "connect-src 'none'"
    assert "'self'" not in directives["script-src"] and directives["frame-ancestors"] == "frame-ancestors 'self'"
    assert r.getheader("x-frame-options") == "SAMEORIGIN"
    r, _ = request(port, "GET", "/")
    assert r.getheader("x-frame-options") == "DENY" and "frame-ancestors 'none'" in r.getheader("content-security-policy")


def page_cookie(port: int, key: str) -> str:
    r, _ = request(port, "GET", f"/?key={key}")
    return r.getheader("set-cookie").split(";")[0]


def post(port: int, cookie: str, route: str, body: dict):
    r, data = request(port, "POST", route, {"Cookie": cookie, "content-type": "application/json", "x-tsugumi": "1"}, json.dumps(body))
    assert r.status == 200, data
    return json.loads(data)


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
    assert post(port, cookie, "/api/project/close", {"page": "a", "path": str(project)}) == {"stopped": False}
    assert not [e for e in pages[2].events if e.get("type") == "projectClosed"]
    # The last page detaches its views while core remains available.
    assert post(port, cookie, "/api/project/close", {"page": "b", "path": str(project)}) == {"stopped": False}
    for _ in range(50):
        if [e for e in pages[2].events if e.get("type") == "projectClosed"]:
            break
        time.sleep(0.1)
    assert {"type": "projectClosed", "project": real} in pages[2].events
    after = post(port, cookie, "/api/env/start", {"path": str(project)})
    assert (after["endpoint"], after["projectId"], after["token"]) == (before["endpoint"], before["projectId"], before["token"])


def test_a_project_taken_in_keeps_its_binary_files(web, tmp_path):
    from conftest import write_project

    _, port, key = web
    cookie = page_cookie(port, key)
    other = tmp_path / "other"
    write_project(other)
    blob = bytes(range(256))
    (other / "blob.bin").write_bytes(blob)
    r = post(port, cookie, "/api/import/project", {"folder": str(other)})
    assert base64.b64decode(r["binary"]["blob.bin"]) == blob
    assert "blob.bin" not in r["files"] and "tsugumi.json" in r["files"]


def test_the_page_is_told_whether_host_folders_can_be_used(web):
    _, port, key = web
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
        for _ in range(100):
            if text in printed():
                return
            time.sleep(0.1)
        raise AssertionError(f"no {text!r} in {printed()!r}")

    def send(data: str):
        post(port, cookie, "/api/terminal/input", {"id": t["id"], "data": data})

    # It has a terminal, of the size the page draws, and programs may use it.
    send('tty; echo "size=$(stty size)" "term=$TERM"; echo 日本語\n')
    until("/dev/")
    until("size=17 91 term=xterm-256color")
    until("日本語")
    post(port, cookie, "/api/terminal/resize", {"id": t["id"], "cols": 100, "rows": 30})
    send('echo "now=$(stty size)"\n')
    until("now=30 100")
    # A key: ⌃C stops what runs, not the shell.
    send("sleep 100\n")
    time.sleep(0.3)
    send("\x03")
    send("echo after-interrupt-$((1 + 1))\n")
    until("after-interrupt-2")
    post(port, cookie, "/api/terminal/stop", {"id": t["id"]})
    for _ in range(100):
        if [e for e in list(page.events) if e.get("type") == "terminal-exit" and e["id"] == t["id"]]:
            break
        time.sleep(0.1)
    else:
        raise AssertionError("the terminal did not end")
