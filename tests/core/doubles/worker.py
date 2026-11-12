"""Programmable worker process for public-RPC scenarios.

The launcher supplies Python's dependencies and generated protocol bindings.
``--package`` names a folder containing scenario.json. Its ``startup`` and
``actions`` arrays describe external behavior; this process never calls core
functions or reads core storage. Startup actions run before either channel is
read, allowing tests to model a package that gets stuck while loading.
"""

from __future__ import annotations

import argparse
import array
import base64
import errno
import json
import os
from pathlib import Path
import select
import socket
import struct
import sys
import threading
import time

import flatbuffers

from Tsugumi.Data.Display import DisplayT
from Tsugumi.Data.DisplayText import DisplayTextT
from Tsugumi.Data.DisplayValue import DisplayValue
from Tsugumi.Data.Failure import FailureT
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Worker.WorkerDisplay import WorkerDisplayT
from Tsugumi.Worker.WorkerFailure import WorkerFailureT
from Tsugumi.Worker.WorkerMessage import WorkerMessage, WorkerMessageT
from Tsugumi.Worker.WorkerOutput import WorkerOutputT
from Tsugumi.Worker.WorkerPayload import WorkerPayload
from Tsugumi.Worker.WorkerResult import WorkerResultT


MAX_FRAME = 16 << 20


def text(value):
    return value.decode() if isinstance(value, bytes) else value


class Channel:
    def __init__(self, fd):
        self.socket = socket.socket(fileno=fd)

    def _read(self, count, descriptors):
        data = bytearray()
        while len(data) < count:
            chunk, ancillary, flags, _ = self.socket.recvmsg(
                count - len(data), socket.CMSG_SPACE(16 * array.array("i").itemsize)
            )
            for level, kind, value in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    fds = array.array("i")
                    fds.frombytes(value[:len(value) - len(value) % fds.itemsize])
                    descriptors.extend(fds)
            if flags & socket.MSG_CTRUNC:
                raise ValueError("truncated descriptor message")
            if not chunk:
                if data:
                    raise EOFError("worker channel closed inside a frame")
                return None
            data.extend(chunk)
        return bytes(data)

    def receive(self):
        descriptors = []
        try:
            header = self._read(4, descriptors)
            if header is None:
                for fd in descriptors:
                    os.close(fd)
                return None
            (size,) = struct.unpack("<I", header)
            if not 0 < size <= MAX_FRAME:
                raise ValueError(f"invalid worker frame size: {size}")
            frame = self._read(size, descriptors)
            if frame is None or frame[4:8] != b"TSW1":
                raise ValueError("invalid worker message")
            message = WorkerMessageT.InitFromObj(WorkerMessage.GetRootAs(frame))
            return message, descriptors
        except BaseException:
            for fd in descriptors:
                os.close(fd)
            raise

    def send(self, call_id, kind, value):
        message = WorkerMessageT(callId=call_id, valueType=kind, value=value)
        builder = flatbuffers.Builder(1024)
        builder.Finish(message.Pack(builder), file_identifier=b"TSW1")
        frame = bytes(builder.Output())
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
        self.cancelled = set()
        self.calls = 0
        self.call_id = 0
        self.call = None

    def control_loop(self):
        try:
            while packet := self.control.receive():
                message, fds = packet
                for fd in fds:
                    os.close(fd)
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
            marker.write_text(json.dumps({"call": self.call_id, "pid": os.getpid()}))
        release = action.get("release")
        if not forever and not release:
            raise ValueError("gate needs a release path")
        deadline = time.monotonic() + float(action.get("timeout", 10))
        while forever or not Path(release).exists():
            # Startup happens before control_loop: observe only EOF, without
            # consuming a cancellation that belongs to the first call.
            if self.call is None and select.select([self.control.socket], [], [], 0)[0]:
                if not self.control.socket.recv(1, socket.MSG_PEEK):
                    os._exit(0)
            if not action.get("ignore_cancel", False):
                self.check_cancel()
            if not forever and time.monotonic() >= deadline:
                raise TimeoutError(f"worker gate was not released: {release}")
            with self.condition:
                self.condition.wait(0.01)
        self.check_cancel()

    def failure(self, kind, message):
        failure = FailureT(kind=getattr(FailureKind, kind), args=[], diagnostic=message)
        self.data.send(self.call_id, WorkerPayload.WorkerFailure, WorkerFailureT(failure=failure))

    def output_path(self, relative):
        if self.call is None or not self.call.outputDirectory:
            raise ValueError("file actions require a File or Directory output")
        relative = Path(relative)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("output action path must stay inside the output directory")
        return Path(text(self.call.outputDirectory)) / relative

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
                    self.condition.wait_for(lambda: self.call_id in self.cancelled, timeout=10)
                self.failure(action.get("kind", "worker_failed"), action.get("message", "scripted worker failure"))
                return False
            elif operation == "failure":
                self.failure(action.get("kind", "worker_failed"), action.get("message", "scripted worker failure"))
                return False
            elif operation == "output":
                sources = {"calls": self.calls, "invocation": self.call.invocation, "pid": os.getpid()}
                value = sources[action["source"]] if "source" in action else action["value"]
                outputs[action["port"]] = scalar(action["type"], value)
            elif operation == "raw":
                outputs[action["port"]] = base64.b64decode(action["base64"], validate=True)
            elif operation == "echo":
                value = next(v for v in self.call.inputs if text(v.name) == action["input"])
                outputs[action["port"]] = bytes(value.value.bytes)
            elif operation in ("file", "directory"):
                destination = self.output_path(action["path"])
                if operation == "file":
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    destination.write_text(action.get("text", ""))
                else:
                    destination.mkdir(parents=True, exist_ok=True)
                    for relative, contents in action.get("files", {}).items():
                        member = self.output_path(str(Path(action["path"]) / relative))
                        member.parent.mkdir(parents=True, exist_ok=True)
                        member.write_text(contents)
                outputs[action["port"]] = scalar("string", str(destination))
            elif operation == "symlink":
                destination = self.output_path(action["path"])
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.symlink_to(action["target"])
            elif operation == "display_text":
                display = DisplayT(
                    card=self.call.card, runId=self.call.runId, invocation=self.call.invocation,
                    revision=1, valueType=DisplayValue.DisplayText,
                    value=DisplayTextT(text=action["text"]),
                )
                self.data.send(self.call_id, WorkerPayload.WorkerDisplay, WorkerDisplayT(display=display))
            else:
                raise ValueError(f"unknown worker action: {operation}")
        return True

    def serve(self):
        if not self.actions(self.scenario.get("startup", []), {}):
            return
        threading.Thread(target=self.control_loop, daemon=True).start()
        while packet := self.data.receive():
            message, received = packet
            installed = []
            try:
                if message.valueType != WorkerPayload.WorkerCall or not message.callId or message.callId <= self.call_id:
                    raise ValueError("expected an ordered worker call")
                self.call_id = message.callId
                self.call = message.value
                self.calls += 1
                slots = [] if self.call.passedFds is None else list(self.call.passedFds)
                if len(slots) != len(received):
                    raise ValueError("worker directory descriptors missing")
                for descriptor, slot in zip(received, slots):
                    if slot < 5:
                        raise ValueError("worker directory descriptor would replace a channel")
                    try:
                        os.fstat(slot)
                    except OSError as error:
                        if error.errno != errno.EBADF:
                            raise
                    else:
                        raise ValueError("worker directory descriptor is already occupied")
                    os.dup2(descriptor, slot)
                    installed.append(slot)
                for descriptor in received:
                    os.close(descriptor)
                received.clear()
                outputs = {}
                if self.actions(self.scenario.get("actions", []), outputs):
                    self.check_cancel()
                    result = WorkerResultT(outputs=[WorkerOutputT(name=port, bytes=value) for port, value in outputs.items()])
                    self.data.send(self.call_id, WorkerPayload.WorkerResult, result)
            except Cancelled:
                self.failure("cancelled", "scripted worker observed cancellation")
            finally:
                for descriptor in received + installed:
                    os.close(descriptor)
                with self.condition:
                    self.cancelled.discard(self.call_id)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", required=True)
    # The real launcher also supplies shell-step arguments. This double only
    # runs the explicit scenario; shell behavior is exercised with the real worker.
    parser.add_argument("--bash")
    parser.add_argument("--pre")
    parser.add_argument("--run")
    parser.add_argument("--post")
    options = parser.parse_args()
    scenario = json.loads((Path(options.package) / "scenario.json").read_text())
    Worker(scenario).serve()


if __name__ == "__main__":
    main()
