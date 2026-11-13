"""A real agentd process and its public, user-authenticated Host socket."""
import importlib
import json
import os
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path

import flatbuffers

from .daemon import BUILD
from .remote_worker import exact, string
from Tsugumi.Host.Message import MessageT
from Tsugumi.Host.Payload import Payload
from Tsugumi.Host.ReplyPayload import ReplyPayload
from Tsugumi.Host.Request import RequestT
from Tsugumi.Host.RequestPayload import RequestPayload


class Host:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.directory.mkdir(mode=0o700)
        self.log = (self.directory / "agent.log").open("wb")
        self.socket_directory = tempfile.TemporaryDirectory(prefix="tsg-host-", dir="/tmp")
        self.process = subprocess.Popen([
            os.environ.get("TSUGUMI_AGENTD", str(BUILD / "core/tsugumi-agentd")),
            "--state-dir", str(self.directory), "--no-websocket",
            "--unix", str(Path(self.socket_directory.name) / "rpc"),
        ], stdout=self.log, stderr=self.log)
        self.socket = None
        self.stream = None
        self.request_id = 0
        try:
            until = time.monotonic() + 15
            while time.monotonic() < until:
                if self.process.poll() is not None:
                    raise RuntimeError((self.directory / "agent.log").read_text())
                try:
                    self.ready = json.loads((self.directory / "agent.ready").read_text())
                    break
                except FileNotFoundError:
                    time.sleep(0.05)
            else:
                raise TimeoutError("agentd did not start")
            self.socket = socket.socket(socket.AF_UNIX)
            self.socket.settimeout(30)
            self.socket.connect(self.ready["host_socket"])
            self.stream = self.socket.makefile("rwb", buffering=0)
            self.call("HelloRequest", token=self.ready["user_token"], clientId="remote-test", protocol=1)
        except BaseException:
            self.close()
            raise

    def call(self, name, **fields):
        namespace = "Service" if name == "HelloRequest" else "Host"
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
        self.socket.sendall(struct.pack("<I", len(data)) + data)
        size, = struct.unpack("<I", exact(self.stream, 4))
        assert 0 < size <= 16 << 20
        response = MessageT.InitFromPackedBuf(exact(self.stream, size))
        assert response.requestId == self.request_id and response.payloadType == Payload.Reply
        reply = response.payload
        if reply.valueType == ReplyPayload.Failure:
            raise RuntimeError(string(reply.value.code) + ": " + string(reply.value.message))
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

    def close(self):
        if self.stream:
            self.stream.close()
        if self.socket:
            self.socket.close()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.log.close()
        self.socket_directory.cleanup()
