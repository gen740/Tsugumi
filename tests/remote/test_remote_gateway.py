"""Transport, authentication, capability and listen policy of the execution daemon."""

import contextlib
import http.client
import io
import os
import socket
import struct
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import remote_gateway as rg
from remote_fixtures import (
    NATIVE, SCOPE_A, TOKEN, DaemonCase, FakeCgroups, FakeDriver, decode, frame, make_daemon, sha,
)


class FramingTests(DaemonCase):
    def test_malformed_frames_are_bounded(self):
        job = "a" * 64
        scope = {"project_scope": SCOPE_A, "server_id": self.daemon.server_id}
        cases = [
            b"", b"\x00\x00", struct.pack(">I", 0), struct.pack(">I", 65537) + b"{}",
            struct.pack(">I", 10) + b"{}", struct.pack(">I", 3) + b"{x}",
            struct.pack(">I", 3) + b"[1]", struct.pack(">I", 2) + b"\xff\xfe",
            frame({"version": True, "operation": "probe"}),
            frame({"version": 2, "operation": "probe"}),
            frame({"version": 1, "operation": "exec"}),
            frame({"version": 1, "operation": "probe", "command": "rm -rf /"}),
            frame({"version": 1, "operation": "probe", "service": "a; reboot"}),
            frame({"version": 1, "operation": "status", "job_id": "../x", **scope}),
            frame({"version": 1, "operation": "status", "job_id": job, "size": 1, **scope}),
            frame({"version": 1, "operation": "put_blob", "sha256": job, **scope}),
            frame({"version": 1, "operation": "submit", "job_id": job, "size": 1 << 20, **scope}),
            frame({"version": 1, "operation": "submit", "job_id": job, "size": 10, **scope}, b"{}"),
            frame({"version": 1, "operation": "status", "job_id": job, **scope}, b"trailing"),
            struct.pack(">I", 25) + b'{"version":1,"version":1}',
        ]
        for data in cases:
            with self.subTest(data=data[:40]):
                raw, handle, size = rg.respond(self.daemon, rg.UploadBody(io.BytesIO(data), len(data)),
                                               self.daemon.server_id)
                resp, rest = decode(raw)
                self.assertEqual((handle, size, rest), (None, 0, b""))
                self.assertEqual((resp["version"], resp["ok"]), (1, False))
                self.assertLessEqual(set(resp), {"version", "ok", "error", "code"})
                self.assertTrue(0 < len(resp["error"]) <= rg.MAX_DIAGNOSTIC)
                self.assertNotIn("Traceback", resp["error"])
        self.assertEqual(self.driver.spawned, [])

    def test_binary_round_trip(self):
        data = bytes(range(256)) * 5000  # spans several chunks
        self.assertEqual(self.put(data), sha(data))
        reply, blob = self.call({"version": 1, "operation": "get_blob", "sha256": sha(data),
                                 "instance_id": self.journal().instance_id,
                                 "project_scope": SCOPE_A, "server_id": self.daemon.server_id})
        self.assertEqual((reply["ok"], reply["size"], blob), (True, len(data), data))
        self.assertFalse(self.scoped("put_blob", body=b"abc", sha256="0" * 64, size=3)["ok"])
        self.assertFalse(self.scoped("get_blob", sha256="1" * 64)["ok"])


class CapabilityTests(DaemonCase):
    def test_unscoped_probe_reports_capabilities_only(self):
        reply, _ = self.call({"version": 1, "operation": "probe"})
        self.assertEqual(reply, {
            "version": 1, "ok": True, "server_id": self.daemon.server_id, "native_system": NATIVE,
            "systems": [NATIVE], "worker_contract": 1, "executor": "daemon",
            "isolation": "pid_namespace", "resource_limits": False, "max_jobs": 8})
        self.assertRegex(self.daemon.server_id, r"^[0-9a-f]{64}$")
        self.assertFalse((self.root / "projects").exists())

    def test_server_id_is_persisted_per_state_root(self):
        again = make_daemon(self.root, FakeDriver())
        self.assertEqual(again.server_id, self.daemon.server_id)
        other = make_daemon(self.root.parent / "other", FakeDriver())
        self.assertNotEqual(other.server_id, self.daemon.server_id)
        # A host pinned to the old server sees the replacement as a different instance.
        reply = self.scoped("probe", daemon=other, server_id=self.daemon.server_id)
        self.assertEqual((reply["ok"], reply["code"]), (False, "instance_changed"))
        reply, _ = self.call({"version": 1, "operation": "probe", "server_id": self.daemon.server_id}, daemon=other)
        self.assertEqual(reply["code"], "instance_changed")

    def test_scope_requires_server_pin(self):
        reply, _ = self.call({"version": 1, "operation": "probe", "project_scope": SCOPE_A})
        self.assertEqual((reply["ok"], reply["code"]), (False, "request_rejected"))
        reply = self.scoped("probe")
        self.assertEqual(reply["instance_id"], self.journal().instance_id)
        self.assertEqual(reply["server_id"], self.daemon.server_id)
        reply = self.scoped("probe", server_id="f" * 64)
        self.assertEqual(reply["code"], "instance_changed")
        for operation, fields in (("status", {"job_id": "c" * 64}), ("get_blob", {"sha256": "c" * 64})):
            reply, _ = self.call({"version": 1, "operation": operation, **fields})
            self.assertEqual(reply["code"], "request_rejected")

    def test_limits_need_delegated_cgroups(self):
        job = "1" * 64
        reply = self.submit(job, self.spec(limits=(1000, 1 << 30)))
        self.assertEqual(reply["code"], "request_rejected")
        self.assertIn("--cgroup-root", reply["error"])
        self.assertIsNone(self.journal().status(job))
        half = self.spec().replace(b'"timeout_seconds"', b'"cpu_millis": 1000, "timeout_seconds"')
        self.assertEqual(self.submit("2" * 64, half)["code"], "request_rejected")
        self.assertEqual(self.submit("3" * 64, self.spec())["state"], "queued")

    def test_limits_accepted_with_cgroups(self):
        daemon = make_daemon(self.root, FakeDriver(), cgroups=FakeCgroups(), max_jobs=3)
        reply, _ = self.call({"version": 1, "operation": "probe"}, daemon=daemon)
        self.assertEqual((reply["resource_limits"], reply["max_jobs"]), (True, 3))
        digest = self.put(b"limited", scope=SCOPE_A)
        self.assertEqual(self.submit("4" * 64, self.spec(digest, limits=(1000, 1 << 30)), daemon=daemon)["state"],
                         "queued")
        low = self.submit("5" * 64, self.spec(digest, limits=(1000, 1)), daemon=daemon)
        self.assertEqual(low["code"], "request_rejected")


class HttpTests(DaemonCase):
    def setUp(self):
        super().setUp()
        self.enterContext(patch.object(rg.RpcHandler, "log_message", lambda *_: None))
        self.server = rg.RpcServer(("127.0.0.1", 0), self.daemon)  # loopback, ephemeral port
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.port = self.server.server_address[1]

    def post(self, data, token=TOKEN, path="/rpc", conn=None, method="POST"):
        conn = conn or http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        headers = {} if token is None else {"Authorization": "Bearer " + token.decode()}
        if data and (server_id := decode(data)[0].get("server_id")):
            headers["X-Tsugumi-Server"] = server_id
        conn.request(method, path, body=data, headers=headers)
        resp = conn.getresponse()
        return resp.status, resp.read()

    def raw(self, request):
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as sock:
            sock.sendall(request)
            chunks = []
            while chunk := sock.recv(65536):
                chunks.append(chunk)
        return b"".join(chunks)

    def probe(self):
        return frame({"version": 1, "operation": "probe"})

    def test_authentication(self):
        for token in (None, b"0" * 64, TOKEN[:-1], TOKEN.upper()):
            with self.subTest(token=token):
                status, body = self.post(self.probe(), token=token)
                self.assertEqual((status, body), (401, b""))
        status, body = self.post(self.probe())
        self.assertEqual(status, 200)
        self.assertEqual(decode(body)[0]["server_id"], self.daemon.server_id)
        twice = b"Authorization: Bearer " + TOKEN + b"\r\n"
        reply = self.raw(b"POST /rpc HTTP/1.1\r\nHost: x\r\n" + twice * 2 + b"Content-Length: 0\r\n\r\n")
        self.assertTrue(reply.startswith(b"HTTP/1.1 401"))

    def test_transport_rejections(self):
        self.assertEqual(self.post(self.probe(), path="/")[0], 404)
        self.assertEqual(self.post(None, method="GET")[0], 405)
        auth = b"Authorization: Bearer " + TOKEN + b"\r\n"
        head = b"POST /rpc HTTP/1.1\r\nHost: x\r\n" + auth
        self.assertTrue(self.raw(head + b"\r\n").startswith(b"HTTP/1.1 411"))
        self.assertTrue(self.raw(head + b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n").startswith(b"HTTP/1.1 411"))
        self.assertTrue(self.raw(head + b"Content-Length: -1\r\n\r\n").startswith(b"HTTP/1.1 400"))
        # Unauthenticated requests are rejected before the body is read.
        big = b"POST /rpc HTTP/1.1\r\nHost: x\r\nContent-Length: 1000000000\r\n\r\n"
        self.assertTrue(self.raw(big).startswith(b"HTTP/1.1 401"))

    def test_streams_blobs_across_requests(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        data = os.urandom(3 << 20)
        pin = {"project_scope": SCOPE_A, "server_id": self.daemon.server_id,
               "instance_id": self.journal().instance_id}
        status, body = self.post(frame({"version": 1, "operation": "put_blob", "sha256": sha(data),
                                        "size": len(data), **pin}, data), conn=conn)
        self.assertEqual((status, decode(body)[0]["ok"]), (200, True))
        status, body = self.post(frame({"version": 1, "operation": "get_blob", "sha256": sha(data), **pin}), conn=conn)
        reply, rest = decode(body)
        self.assertEqual((status, reply["size"], rest), (200, len(data), data))

    def test_disconnected_client_keeps_durable_submission(self):
        job, spec = "9" * 64, self.spec()
        body = frame({"version": 1, "operation": "submit", "job_id": job, "size": len(spec),
                      "project_scope": SCOPE_A, "server_id": self.daemon.server_id,
                      "instance_id": self.journal().instance_id}, spec)
        request = (b"POST /rpc HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer " + TOKEN
                   + b"\r\nX-Tsugumi-Server: " + self.daemon.server_id.encode()
                   + b"\r\nContent-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as sock:
            sock.sendall(request)  # and leave without reading the receipt
        for _ in range(100):
            if self.journal().status(job) is not None:
                break
            time.sleep(0.05)
        self.assertEqual(self.journal().status(job)["fingerprint"], sha(spec))
        self.assertEqual(self.scoped("cancel", job_id=job, fingerprint=sha(spec))["state"], "cancelled")
        self.daemon.scheduler.tick()
        self.assertEqual(self.driver.spawned, [])


class PolicyTests(unittest.TestCase):
    def test_listen_policy(self):
        self.assertEqual(rg.check_listen_policy("127.0.0.1", False), "127.0.0.1")
        self.assertEqual(rg.check_listen_policy("localhost", False), "127.0.0.1")
        self.assertEqual(rg.check_listen_policy("::1", False), "::1")
        self.assertEqual(rg.check_listen_policy("192.0.2.10", True), "192.0.2.10")
        for host, tls in (("0.0.0.0", False), ("::", False), ("192.0.2.10", False), ("example.org", True)):
            with self.subTest(host=host), self.assertRaises(rg.GatewayError):
                rg.check_listen_policy(host, tls)

    def token_file(self, data, mode=0o600):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / "token"
        path.write_bytes(data)
        os.chmod(path, mode)
        return str(path)

    def test_token_file(self):
        self.assertEqual(rg.load_token(self.token_file(TOKEN + b"\n")), TOKEN)
        self.assertEqual(rg.load_token(self.token_file(TOKEN)), TOKEN)
        for data, mode in ((TOKEN.upper(), 0o600), (TOKEN[:-2], 0o600), (TOKEN + b"\n\n", 0o600),
                           (TOKEN + b"00", 0o600), (TOKEN, 0o640), (TOKEN, 0o604)):
            with self.subTest(data=data, mode=oct(mode)), self.assertRaises(rg.GatewayError) as caught:
                rg.load_token(self.token_file(data, mode))
            self.assertNotIn(TOKEN.decode(), str(caught.exception).lower())

    def test_refuses_insecure_configuration_before_listening(self):
        token = self.token_file(TOKEN)
        cases = (["--host", "0.0.0.0", "--token-file", token],
                 ["--host", "192.0.2.10", "--token-file", token],
                 ["--tls-cert", token, "--token-file", token])
        for argv in cases:
            err = io.StringIO()
            with self.subTest(argv=argv[:2]), contextlib.redirect_stderr(err), \
                    patch.object(rg, "RpcServer", side_effect=AssertionError("must not listen")):
                self.assertEqual(rg.serve_main(argv), 1)
            self.assertNotIn(TOKEN.decode(), err.getvalue())

    def test_single_daemon_per_state_root(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        fd = rg.acquire_singleton(Path(tmp.name))
        self.addCleanup(os.close, fd)
        with self.assertRaises(rg.GatewayError):
            rg.acquire_singleton(Path(tmp.name))


if __name__ == "__main__":
    unittest.main()
