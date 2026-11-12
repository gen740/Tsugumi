import hashlib
import http.client
import io
import json
import socket
import struct
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor

from remote_gateway import Daemon, RpcServer


class RemoteHttpAdmissionTest(unittest.TestCase):
    token = "a" * 64

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.daemon = Daemon(directory.name, token=self.token.encode(), system="x86_64-linux",
                             worker="/unused", unshare="/unused")
        self.server = RpcServer(("127.0.0.1", 0), self.daemon)
        self.addCleanup(self.server.server_close)
        thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join, 3)
        self.addCleanup(self.server.shutdown)

    def expect_continue(self, connection, *, bearer, server_id, size, extra=()):
        headers = [
            "POST /rpc HTTP/1.1",
            "Host: localhost",
            f"Authorization: Bearer {bearer}",
            f"X-Tsugumi-Server: {server_id}",
            f"Content-Length: {size}",
            "Expect: 100-continue",
            "Connection: close",
            *extra,
        ]
        connection.sendall(("\r\n".join(headers) + "\r\n\r\n").encode())

    def test_rejects_upload_before_asking_for_body(self):
        for name, bearer, identity, extra, status in (
            ("wrong bearer", "b" * 64, self.daemon.server_id, (), 401),
            ("replaced daemon", self.token, "b" * 64, (), 409),
            ("duplicate bearer", self.token, self.daemon.server_id, (f"Authorization: Bearer {self.token}",), 401),
            ("duplicate content length", self.token, self.daemon.server_id, ("Content-Length: 256",), 400),
        ):
            with self.subTest(name=name):
                with socket.create_connection(self.server.server_address, timeout=3) as connection:
                    with connection.makefile("rb") as response:
                        self.expect_continue(connection, bearer=bearer, server_id=identity, size=256, extra=extra)
                        # The first response is final while no frame bytes have been sent.
                        self.assertEqual(int(response.readline().split()[1]), status)

    def test_accepts_upload_after_verifying_the_pinned_daemon(self):
        contents = b"admitted blob"
        digest = hashlib.sha256(contents).hexdigest()
        scope = "c" * 64
        journal = self.daemon.project(scope).journal
        header = json.dumps({
            "version": 1,
            "operation": "put_blob",
            "project_scope": scope,
            "server_id": self.daemon.server_id,
            "instance_id": journal.instance_id,
            "sha256": digest,
            "size": len(contents),
        }).encode()
        frame = struct.pack(">I", len(header)) + header + contents
        with socket.create_connection(self.server.server_address, timeout=3) as connection:
            with connection.makefile("rb") as response:
                self.expect_continue(connection, bearer=self.token, server_id=self.daemon.server_id, size=len(frame))
                self.assertEqual(response.readline().split()[1], b"100")
                self.assertEqual(response.readline(), b"\r\n")
                connection.sendall(frame)
                self.assertEqual(response.readline().split()[1], b"200")
        with journal.open_blob(digest) as stored:
            self.assertEqual(stored.read(), contents)

    def test_parallel_submissions_share_one_receipt(self):
        scope = "c" * 64
        job_id = "d" * 64
        journal = self.daemon.project(scope).journal
        invocation = b"input to the worker"
        digest = hashlib.sha256(invocation).hexdigest()
        journal.put_blob(digest, len(invocation), io.BytesIO(invocation))
        spec = {
            "system": "x86_64-linux",
            "derivation": "/nix/store/" + "0" * 32 + "-card.drv",
            "output": "/nix/store/" + "0" * 32 + "-card",
            "invocation_sha256": digest,
            "input_blobs": [digest],
            "timeout_seconds": 60,
        }

        def submit(candidate):
            body = json.dumps(candidate).encode()
            header = json.dumps({
                "version": 1,
                "operation": "submit",
                "project_scope": scope,
                "server_id": self.daemon.server_id,
                "instance_id": journal.instance_id,
                "job_id": job_id,
                "size": len(body),
            }).encode()
            frame = struct.pack(">I", len(header)) + header + body
            connection = http.client.HTTPConnection(*self.server.server_address, timeout=3)
            try:
                connection.request("POST", "/rpc", frame, {
                    "Authorization": "Bearer " + self.token,
                    "X-Tsugumi-Server": self.daemon.server_id,
                })
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                size, = struct.unpack(">I", response.read(4))
                return json.loads(response.read(size))
            finally:
                connection.close()

        with ThreadPoolExecutor(max_workers=6) as pool:
            replies = list(pool.map(submit, [spec] * 6))
        self.assertTrue(all(reply["ok"] and reply["state"] == "queued" for reply in replies))
        self.assertEqual(len({reply["fingerprint"] for reply in replies}), 1)
        conflicting = submit({**spec, "timeout_seconds": 61})
        self.assertEqual(conflicting["code"], "request_rejected")
        self.assertTrue(journal.claim(job_id, "first-supervisor"))
        self.assertFalse(journal.claim(job_id, "second-supervisor"))
