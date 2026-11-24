"""Programmable worker process for public-RPC scenarios.

The launcher supplies Python's dependencies and generated protocol bindings.
``--package`` names a folder containing scenario.json. Its ``startup`` and
``actions`` arrays describe external behavior; this process never calls core
functions or reads core storage. Startup actions run before either channel is
read, allowing tests to model a runtime that gets stuck during preparation.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import select
import socket
import struct
import sys
import threading
from uuid import UUID

import flatbuffers

from Tsugumi.Data.Failure import FailureT
from Tsugumi.Data.FailureKind import FailureKind
from tsugumi.Artifact import ArtifactT
from tsugumi.ArtifactId import ArtifactIdT
from Tsugumi.Worker.WorkerArtifactCreate import WorkerArtifactCreateT
from Tsugumi.Worker.WorkerArtifactOpen import WorkerArtifactOpenT
from Tsugumi.Worker.WorkerDisplay import WorkerDisplayT
from Tsugumi.Worker.WorkerFailure import WorkerFailureT
from Tsugumi.Worker.WorkerMessage import WorkerMessage, WorkerMessageT
from Tsugumi.Worker.WorkerOutput import WorkerOutputT
from Tsugumi.Worker.WorkerPayload import WorkerPayload
from Tsugumi.Worker.WorkerResult import WorkerResultT

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from harness.file_events import wait_for_file  # noqa: E402


MAX_FRAME = (16 << 20) + 65536


def text(value):
    return value.decode() if isinstance(value, bytes) else value


class Channel:
    def __init__(self, fd):
        self.socket = socket.socket(fileno=fd)
        self.lock = threading.Lock()

    def _read(self, count):
        data = bytearray()
        while len(data) < count:
            chunk = self.socket.recv(count - len(data))
            if not chunk:
                if data:
                    raise EOFError("worker channel closed inside a frame")
                return None
            data.extend(chunk)
        return bytes(data)

    def receive(self):
        header = self._read(4)
        if header is None:
            return None
        (size,) = struct.unpack("<I", header)
        if not 0 < size <= MAX_FRAME:
            raise ValueError(f"invalid worker frame size: {size}")
        frame = self._read(size)
        if frame is None or frame[4:8] != b"TSW1":
            raise ValueError("invalid worker message")
        return WorkerMessageT.InitFromObj(WorkerMessage.GetRootAs(frame))

    def send(self, call_id, kind, value):
        message = WorkerMessageT(callId=call_id, valueType=kind, value=value)
        builder = flatbuffers.Builder(1024)
        builder.Finish(message.Pack(builder), file_identifier=b"TSW1")
        frame = bytes(builder.Output())
        with self.lock:
            self.socket.sendall(struct.pack("<I", len(frame)) + frame)


def scalar(kind, value):
    """Build a one-field port message; its schema is supplied by the project."""
    builder = flatbuffers.Builder(128)
    builder.ForceDefaults(True)
    string = builder.CreateString(str(value)) if kind == "string" else None
    builder.StartObject(1)
    if kind == "string":
        builder.PrependUOffsetTRelativeSlot(0, string, 0)
    else:
        operations = {
            "int64": builder.PrependInt64Slot,
            "uint64": builder.PrependUint64Slot,
            "double": builder.PrependFloat64Slot,
            "bool": builder.PrependBoolSlot,
        }
        operations[kind](0, value, 0)
    builder.Finish(builder.EndObject())
    return bytes(builder.Output())


class Cancelled(Exception):
    pass


class Worker:
    def __init__(self, scenario):
        self.scenario = scenario
        self.data = Channel(3)
        self.control = Channel(4)
        self.condition = threading.Condition()
        self.changed, self.wake = socket.socketpair()
        self.wake.setblocking(False)
        self.cancelled = set()
        self.calls = 0
        self.call_id = 0
        self.call = None
        self.created = []

    def control_loop(self):
        try:
            while message := self.control.receive():
                if message.valueType == WorkerPayload.WorkerCancel:
                    with self.condition:
                        self.cancelled.add(message.callId)
                        self.condition.notify_all()
        except (EOFError, OSError, ValueError):
            pass
        # An intentionally uncooperative call still belongs to its daemon.
        # Do not leave it behind after a scenario crashes or kills that owner.
        os._exit(0)

    def check_cancel(self):
        with self.condition:
            if self.call_id in self.cancelled:
                raise Cancelled()

    def wait(self, action, *, forever=False):
        entered = action.get("entered")
        if entered:
            marker = Path(entered)
            marker.parent.mkdir(parents=True, exist_ok=True)
            temporary = marker.with_suffix(".tmp")
            temporary.write_text(json.dumps({"call": self.call_id, "pid": os.getpid(), "parent_pid": os.getppid()}))
            temporary.replace(marker)
        release = action.get("release")
        if not forever and not release:
            raise ValueError("gate needs a release path")
        ignore_cancel = action.get("ignore_cancel", False)
        if forever:
            if ignore_cancel:
                # The control thread exits the worker when the owner closes
                # the socket, even for a deliberately uncooperative call.
                threading.Event().wait()
            with self.condition:
                self.condition.wait_for(lambda: self.call_id in self.cancelled)
        else:
            marker = Path(release)
            while not marker.exists():
                self.check_cancel()
                if wait_for_file(marker, exited=self.changed.fileno()) is not None:
                    break
                self.changed.recv(4096)
        self.check_cancel()

    def artifact_request(self, kind, value, expected):
        self.data.send(self.call_id, kind, value)
        reply = self.data.receive()
        if reply is None or reply.callId != self.call_id or reply.valueType != expected:
            raise ValueError("invalid artifact effect reply")
        return reply.value

    def failure(self, kind, message):
        failure = FailureT(kind=getattr(FailureKind, kind), args=[], diagnostic=message)
        self.data.send(self.call_id, WorkerPayload.WorkerFailure, WorkerFailureT(failure=failure))

    def actions(self, actions, outputs):
        for action in actions:
            self.check_cancel()
            operation = action["op"]
            if operation == "gate":
                self.wait(action)
            elif operation == "hang":
                self.wait(action, forever=True)
            elif operation == "exit":
                os._exit(int(action.get("code", 17)))
            elif operation == "malformed":
                self.data.socket.sendall(struct.pack("<I", 8) + b"not-tsw1")
                return False
            elif operation == "log":
                stream = sys.stderr if action.get("stream", "stdout") == "stderr" else sys.stdout
                print(action["text"], file=stream, flush=True)
            elif operation == "failure_after_cancel":
                # A failure of its own that only happens to arrive after the
                # host asked the call to stop.
                marker = Path(action["entered"])
                marker.parent.mkdir(parents=True, exist_ok=True)
                marker.write_text(json.dumps({"call": self.call_id, "pid": os.getpid()}))
                with self.condition:
                    self.condition.wait_for(lambda: self.call_id in self.cancelled)
                self.failure(action.get("kind", "worker_failed"), action.get("message", "scripted worker failure"))
                return False
            elif operation == "failure":
                self.failure(action.get("kind", "worker_failed"), action.get("message", "scripted worker failure"))
                return False
            elif operation == "output":
                sources = {"calls": self.calls, "pid": os.getpid()}
                value = sources[action["source"]] if "source" in action else action["value"]
                outputs[action["port"]] = WorkerOutputT(name=action["port"], bytes=scalar(action["type"], value))
            elif operation == "echo":
                value = next(v for v in self.call.inputs if text(v.name) == action["input"])
                outputs[action["port"]] = WorkerOutputT(
                    name=action["port"], bytes=bytes(value.bytes) if value.bytes is not None else b"")
            elif operation in ("file", "directory"):
                reply = self.artifact_request(
                    WorkerPayload.WorkerArtifactCreate,
                    WorkerArtifactCreateT(flags=1 if action.get("keep", True) else 0),
                    WorkerPayload.WorkerArtifactCreated)
                if reply.id is None:
                    raise ValueError("artifact creation refused")
                counter, artifact = reply.counter, reply.id
                self.created.append(counter)
                root = Path(text(self.call.artifactDirectory)) / str(counter)
                destination = root / action["path"]
                if not destination.is_relative_to(root) or ".." in destination.parts:
                    raise ValueError("artifact member escapes staging")
                if operation == "file":
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_text(action.get("text", ""))
                else:
                    destination.mkdir(parents=True, exist_ok=True)
                    for relative, contents in action.get("files", {}).items():
                        member = destination / relative
                        if not member.is_relative_to(root) or ".." in member.parts:
                            raise ValueError("artifact member escapes staging")
                        member.parent.mkdir(parents=True, exist_ok=True)
                        member.write_text(contents)
                builder = flatbuffers.Builder(64)
                builder.Finish(ArtifactT(id=artifact).Pack(builder))
                outputs[action["port"]] = WorkerOutputT(name=action["port"], bytes=bytes(builder.Output()))
                if "uuid_file" in action:
                    Path(action["uuid_file"]).write_text(str(UUID(bytes=bytes(artifact.bytes))))
            elif operation == "open_artifact":
                key = ArtifactIdT(bytes=list(UUID(Path(action["uuid_file"]).read_text()).bytes))
                reply = self.artifact_request(WorkerPayload.WorkerArtifactOpen,
                    WorkerArtifactOpenT(id=key), WorkerPayload.WorkerArtifactOpened)
                if reply.path is None:
                    self.failure("input_missing", "artifact unavailable")
                    return False
                data = (Path(text(reply.path)) / action["member"]).read_text()
                outputs[action["port"]] = WorkerOutputT(name=action["port"], bytes=scalar("string", data))
            elif operation == "artifact_symlink":
                root = Path(text(self.call.artifactDirectory)) / str(self.created[-1])
                (root / action["name"]).symlink_to(action["target"])
            elif operation == "display_text":
                display = WorkerDisplayT(view="test", bytes=action["text"].encode())
                self.data.send(0, WorkerPayload.WorkerDisplay, display)
            else:
                raise ValueError(f"unknown worker action: {operation}")
        return True

    def serve(self):
        threading.Thread(target=self.control_loop, daemon=True).start()
        if not self.actions(self.scenario.get("startup", []), {}):
            return
        while message := self.data.receive():
            try:
                if message.valueType == WorkerPayload.WorkerStart and self.calls == 0:
                    os.environ.update({text(entry.name): text(entry.value)
                                       for entry in message.value.environment})
                    continue
                if message.valueType != WorkerPayload.WorkerCall:
                    raise ValueError("expected a worker call")
                self.call_id = message.callId
                self.call = message.value
                self.created = []
                self.calls += 1
                outputs = {}
                if self.actions(self.scenario.get("actions", []), outputs):
                    self.check_cancel()
                    result = WorkerResultT(outputs=list(outputs.values()))
                    self.data.send(self.call_id, WorkerPayload.WorkerResult, result)
            except Cancelled:
                self.failure("cancelled", "scripted worker observed cancellation")
            finally:
                with self.condition:
                    self.cancelled.discard(self.call_id)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True)
    options = parser.parse_args()
    scenario = json.loads((Path(options.package) / "scenario.json").read_text())
    Worker(scenario).serve()


if __name__ == "__main__":
    main()
