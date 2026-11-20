"""The Web UI's origin, which hostd serves: only the page opened through the
URL `tsugumi web` printed (its key, then a cookie) reaches the Host token and
the RPC sockets, nothing outside the page is served, every answer forbids
framing and sniffing, and pages are routed by hostd. Needs the built command,
hostd and ui/web/dist (npm run build)."""

from __future__ import annotations

from contextlib import closing, contextmanager
from html.parser import HTMLParser
import http.client
from http.cookies import SimpleCookie
import json
import os
import re
import subprocess
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest

from conftest import CLI, HOSTD, TSUGUMID, WEB_ROOT, write_project
from test_claude_code_plugin import isolated_environment, stop_resident_services

ROOT = Path(__file__).resolve().parents[2]

pytestmark = pytest.mark.skipif(not CLI.exists() or not HOSTD.exists() or not (WEB_ROOT / "index.html").exists(),
                                reason="needs build/cli/tsugumi, tsugumi-hostd and ui/web/dist (cd ui/web && npm run build)")


@contextmanager
def web_host(tmp_path, **environment):
    env = isolated_environment(tmp_path, TSUGUMI_WEB_ROOT=str(WEB_ROOT), **environment)
    try:
        r = subprocess.run([str(CLI), "web", "--no-browser"], capture_output=True, text=True, env=env, timeout=120)
        assert r.returncode == 0, r.stderr
        url = urlparse(re.search(r"Tsugumi web UI: (\S+)", r.stdout).group(1))
        assert url.hostname == "127.0.0.1"
        yield env, url.port, parse_qs(url.query)["key"][0]
    finally:
        stop_resident_services(tmp_path)


@pytest.fixture
def web(tmp_path):
    with web_host(tmp_path) as server:
        yield server


@pytest.fixture(scope="module")
def http_server(tmp_path_factory):
    with web_host(tmp_path_factory.mktemp("web-http")) as server:
        yield server


def request(port: int, method: str, path: str, headers=None, body=None, host=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.request(method, path, body=body, headers={"Host": host or f"127.0.0.1:{port}", **(headers or {})})
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


def page_cookie(port: int, key: str) -> str:
    r, _ = request(port, "GET", f"/?key={key}")
    return r.getheader("set-cookie").split(";")[0]


def host_token(port: int, cookie: str) -> str:
    r, data = request(port, "POST", "/api/host/connect", {"Cookie": cookie, "x-tsugumi": "1"})
    assert r.status == 200, data
    return json.loads(data)["token"]


def page_host(port, cookie):
    from harness.host import HostConnection
    from harness.transport import ws_connect

    token = host_token(port, cookie)
    connection, status = ws_connect(port, f"http://127.0.0.1:{port}", "tsugumi.host.v1",
                                    path="/rpc/host", headers=[("Cookie", cookie)])
    if status.split()[1] != "101":
        connection.close()
        raise AssertionError(status)
    return HostConnection({"user_token": token}, websocket=connection)


def next_event(host, kind: str):
    """The next Host event of one kind; environment states and others pass by."""
    from Tsugumi.Host.EventPayload import EventPayload
    while True:
        event = host.next_event()
        if event.valueType == getattr(EventPayload, kind):
            return event.value


def test_only_the_page_with_the_key_gets_the_host_token(http_server):
    _, port, key = http_server
    assert request(port, "POST", "/api/host/connect", {"x-tsugumi": "1"})[0].status == 401
    assert request(port, "POST", "/api/host/connect", {"Cookie": f"tsugumi_{port}=wrong", "x-tsugumi": "1"})[0].status == 401
    r, _ = request(port, "GET", f"/?key={key}&project=x")
    assert r.status == 302
    location = urlparse(r.getheader("location"))
    assert location.path == "/" and not location.netloc
    query = parse_qs(location.query)
    assert query["project"] == ["x"] and "key" not in query
    session, = SimpleCookie(r.getheader("set-cookie")).values()
    assert session["httponly"] and session["samesite"].lower() == "strict"
    cookie = f"{session.key}={session.value}"
    # Without the header a cross-origin form cannot send, the token stays.
    assert request(port, "POST", "/api/host/connect", {"Cookie": cookie})[0].status == 404
    assert host_token(port, cookie)
    # The redirect never leaves this origin.
    for path in ("//evil.example/x", "/\\evil.example/x"):
        r, _ = request(port, "GET", f"{path}?key={key}")
        assert r.status == 302 and urlparse(r.getheader("location")).netloc == ""


def test_other_host_names_and_files_outside_the_page_are_refused(http_server):
    _, port, _ = http_server
    assert request(port, "GET", "/", host=f"evil.example:{port}")[0].status == 403
    assert request(port, "GET", "/../VERSION")[0].status == 403
    r, data = request(port, "GET", "/%2e%2e/%2e%2e/VERSION")
    assert r.status == 403
    home, page = request(port, "GET", "/")
    assert home.status == 200
    # An unknown path is the page itself (the app routes in the browser).
    r, fallback = request(port, "GET", "/no/such/file")
    assert r.status == 200 and fallback == page
    csp = policy(home)
    assert csp["frame-ancestors"] == {"'none'"}
    # The page reaches its project's tsugumid over a loopback WebSocket.
    assert "ws://127.0.0.1:*" in csp["connect-src"]
    assert home.getheader("x-content-type-options") == "nosniff"
    assert home.getheader("x-frame-options") == "DENY"


def test_the_shared_wasm_client_is_served_and_allowed_by_the_page_policy(http_server):
    _, port, _ = http_server
    response, _ = request(port, "GET", "/")
    scripts = policy(response)["script-src"]
    assert "'wasm-unsafe-eval'" in scripts and "'unsafe-eval'" not in scripts
    response, module = request(port, "GET", "/client/tsugumi-client.wasm")
    assert response.status == 200 and response.getheader("content-type") == "application/wasm"
    assert module[:4] == b"\0asm"
    response, script = request(port, "GET", "/client/tsugumi-client.js")
    assert response.status == 200 and script
    assert response.getheader("content-type").split(";")[0] == "text/javascript"


def test_the_plot_viewer_is_framed_sandboxed_and_reaches_nothing(http_server):
    """/modules/ may be framed by the page alone, always runs sandboxed (an
    opaque origin: no cookie, no RPC), loads nothing and connects nowhere."""
    _, port, _ = http_server
    r, data = request(port, "GET", "/modules/viewer.html")
    assert r.status == 200
    scripts = Scripts()
    scripts.feed(data.decode())
    assert scripts.sources and all(source is None for source in scripts.sources)
    csp = policy(r)
    assert "allow-scripts" in csp["sandbox"] and "allow-same-origin" not in csp["sandbox"]
    assert csp["default-src"] == csp["connect-src"] == {"'none'"}
    assert "'self'" not in csp["script-src"] and csp["frame-ancestors"] == {"'self'"}
    assert csp["frame-src"] == {"'none'"}
    assert r.getheader("x-frame-options") == "SAMEORIGIN"


def test_host_rpc_requires_the_page_cookie_and_origin(web):
    from harness.transport import ws_connect

    _, port, key = web
    cookie = page_cookie(port, key)
    for origin, headers in [
        (f"http://127.0.0.1:{port}", []),
        (None, [("Cookie", cookie)]),
        ("http://another.example", [("Cookie", cookie)]),
    ]:
        connection, status = ws_connect(port, origin, "tsugumi.host.v1", path="/rpc/host", headers=headers)
        with connection:
            assert status.split()[1] == "403"
    with closing(page_host(port, cookie)) as host:
        assert host.call("HostRecentListRequest").projects == []
        home = host.call("HostHomeRequest")
        assert home.home and home.documents


def test_agent_rpc_reaches_the_authenticated_agent_listener(web, tmp_path):
    from harness.transport import ws_connect, ws_frame, ws_read

    _, port, key = web
    cookie = page_cookie(port, key)
    token = host_token(port, cookie)
    connection, status = ws_connect(port, f"http://127.0.0.1:{port}", "tsugumi.agent.v1",
                                    path="/rpc/agent", headers=[("Cookie", cookie)])
    with connection:
        assert status.split()[1] == "101"
        flatc = os.environ.get("TSUGUMI_FLATC", "flatc")
        schema = str(ROOT / "protocol/agent.fbs")

        def call(request_id, kind, value):
            request_file = tmp_path / "request.json"
            request_file.write_text(json.dumps({
                "request_id": request_id, "payload_type": "Request",
                "payload": {"value_type": kind, "value": value},
            }))
            subprocess.run([flatc, "-b", "-o", str(tmp_path), schema, str(request_file)], check=True)
            connection.sendall(ws_frame(request_file.with_suffix(".bin").read_bytes()))
            reply = tmp_path / "reply.bin"
            reply.write_bytes(ws_read(connection))
            subprocess.run([flatc, "-t", "--strict-json", "-o", str(tmp_path), schema, "--", str(reply)], check=True)
            result = json.loads(reply.with_suffix(".json").read_text())
            assert result["request_id"] == request_id and result["payload_type"] == "Reply"
            return result["payload"]["value_type"]

        assert call(1, "HelloRequest", {"token": token}) == "HelloResult"
        assert call(2, "ConversationListRequest", {}) == "ConversationList"


@pytest.mark.skipif(not TSUGUMID.exists(), reason="needs tsugumid")
def test_hostd_routes_projects_to_pages(web, tmp_path):
    """A page on Home is given the project `tsugumi web` shows; a page that
    leaves the last one showing it tells the detached pages."""
    env, port, key = web
    cookie = page_cookie(port, key)
    project = tmp_path / "proj"
    write_project(project)
    real = str(project.resolve())
    with closing(page_host(port, cookie)) as home, closing(page_host(port, cookie)) as child:
        home.call("HostPageRequest")
        r = subprocess.run([str(CLI), "web", "--no-browser", str(project)], capture_output=True, text=True, env=env, timeout=300)
        assert r.returncode == 0, r.stderr
        assert "A Tsugumi page shows it." in r.stdout
        assert next_event(home, "PageShow").project.decode() == real
        home.call("HostPageRequest", project=real)
        # A child workflow's page counts as showing it, never takes another.
        child.call("HostPageRequest", project=real, detached=True)
        home.call("HostPageRequest")
        assert next_event(child, "PageClosed").project.decode() == real
