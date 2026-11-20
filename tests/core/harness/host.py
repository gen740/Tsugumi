"""A real hostd process and its public, user-authenticated Host socket."""
import importlib
import json
import os
import socket
import struct
import subprocess
import tempfile
from collections import deque
from pathlib import Path

import flatbuffers

from .daemon import BUILD
from .file_events import process_file
from .remote_worker import exact, string
from .transport import ws_frame, ws_read
from Tsugumi.Host.Message import MessageT
from Tsugumi.Host.Payload import Payload
from Tsugumi.Host.ReplyPayload import ReplyPayload
from Tsugumi.Host.Request import RequestT
from Tsugumi.Host.RequestPayload import RequestPayload
from Tsugumi.Trace.Client import Client


class HostFailure(RuntimeError):
    def __init__(self, failure):
        self.code = string(failure.code)
        self.request_id = failure.requestId
        super().__init__(string(failure.message))


class HostConnection:
    def __init__(self, ready, *, trace=False, websocket=None):
        self.socket = websocket if websocket is not None else socket.socket(socket.AF_UNIX)
        self.socket.settimeout(30)
        self.stream = None if websocket is not None else self.socket.makefile("rwb", buffering=0)
        self.request_id = 0
        self.events = deque()
        try:
            if self.stream is not None:
                self.socket.connect(ready["host_socket"])
            self.call("HelloRequest", token=ready["user_token"],
                      client=Client.Trace if trace else Client.Unspecified)
        except BaseException:
            HostConnection.close(self)
            raise

    def call(self, name, /, **fields):
        namespace = {"HelloRequest": "Service", "HostTraceRequest": "Trace"}.get(name, "Host")
        value = getattr(importlib.import_module(f"Tsugumi.{namespace}.{name}"), name + "T")()
        for key, item in fields.items():
            if not hasattr(value, key):
                raise AttributeError(f"{name} has no {key}")
            setattr(value, key, item)
        request = RequestT()
        request.valueType = getattr(RequestPayload, name)
        request.value = value
        message = MessageT()
        self.request_id += 1
        message.requestId = self.request_id
        message.payloadType = Payload.Request
        message.payload = request
        builder = flatbuffers.Builder(128)
        builder.Finish(message.Pack(builder), file_identifier=b"TSH1")
        data = bytes(builder.Output())
        self.socket.sendall(ws_frame(data) if self.stream is None else struct.pack("<I", len(data)) + data)
        response = self._receive()
        while response.payloadType == Payload.Event:
            self.events.append(response.payload)
            response = self._receive()
        assert response.requestId == self.request_id and response.payloadType == Payload.Reply
        reply = response.payload
        if reply.valueType == ReplyPayload.Failure:
            raise HostFailure(reply.value)
        # Python's generated native API uses None for absent vectors.
        # All optional Host lists have the same omission-means-empty rule.
        value = reply.value
        for name in ("services", "projects", "models", "demos"):
            if hasattr(value, name) and getattr(value, name) is None:
                setattr(value, name, [])
        for item in getattr(value, "services", []):
            if item.capabilities.systems is None:
                item.capabilities.systems = []
        for item in getattr(value, "demos", []):
            if item.tags is None:
                item.tags = []
        return value

    def _receive(self):
        if self.stream is None:
            return MessageT.InitFromPackedBuf(ws_read(self.socket))
        size, = struct.unpack("<I", exact(self.stream, 4))
        assert 0 < size <= 16 << 20
        return MessageT.InitFromPackedBuf(exact(self.stream, size))

    def next_event(self):
        if self.events:
            return self.events.popleft()
        response = self._receive()
        assert response.payloadType == Payload.Event
        return response.payload

    def close(self):
        if self.stream is not None:
            self.stream.close()
        self.socket.close()


class Host(HostConnection):
    def __init__(self, directory, *, rpc_trace=False, args=()):
        self.directory = Path(directory).resolve()
        self.socket_directory = tempfile.TemporaryDirectory(prefix="th-", dir=os.environ.get("TSUGUMI_SOCKET_DIR", "/tmp"))
        self.log_path = Path(self.socket_directory.name) / "host.log"
        self.log = self.log_path.open("wb")
        self.process = None
        try:
            self.process = subprocess.Popen([
                os.environ.get("TSUGUMI_HOSTD", str(BUILD / "hostd/tsugumi-hostd")),
                "--state-dir", str(self.directory), "--no-websocket",
                "--socket-dir", str(Path(self.socket_directory.name) / "b"),
                "--usage-scratch-dir", str(self.directory / "run-scratch"),
                "--unix", str(Path(self.socket_directory.name) / "rpc"),
                *args,
            ], stdout=self.log, stderr=self.log,
                env=dict(os.environ, TSUGUMI_RPC_TRACE="1" if rpc_trace else "0"))
            ready = process_file(self.process, self.directory / "host.ready")
            if ready is None:
                raise RuntimeError(self.log_path.read_text())
            self.ready = json.loads(ready.read_text())
            super().__init__(self.ready)
        except BaseException:
            self._stop()
            raise

    def close(self):
        super().close()
        self._stop()

    def _stop(self):
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            # Escalate only if a fixture fails to shut down gracefully.
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()
        self.socket_directory.cleanup()
