"""The web UI's host server: only the page opened through the URL it printed
(its key, then a cookie) reaches /api/, a malformed request fails alone, files
outside the page are not served, and every answer forbids framing and
sniffing. Needs node and a built web/dist (npm run build)."""

from __future__ import annotations

import http.client
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / "web" / "dist" / "server.js"

pytestmark = pytest.mark.skipif(not shutil.which("node") or not SERVER.exists(),
                                reason="needs node and web/dist (cd web && npm run build)")


@pytest.fixture
def web(tmp_path):
    env = dict(os.environ, TSUGUMI_WEB_DATA=str(tmp_path / "data"))
    p = subprocess.Popen(["node", str(SERVER), "--port", "0"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    try:
        line = p.stdout.readline()
        m = re.search(r"http://127\.0\.0\.1:(\d+)/\?key=([0-9a-f]+)", line)
        assert m, line
        yield p, int(m.group(1)), m.group(2)
    finally:
        p.terminate()
        p.wait(10)


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
    assert r.getheader("x-content-type-options") == "nosniff"
