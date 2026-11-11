"""A real RPC connection with controllable delivery of individual replies.

Requests always reach the daemon unchanged. Only the selected reply is lost
or held; notifications and unrelated replies keep flowing while it is held.
Each proxy serves one client, so a request id identifies exactly one reply.
"""

from __future__ import annotations

import socket
import struct
import threading
import time
import uuid
from pathlib import Path
from typing import Callable


MAX_FRAME_BYTES = 16 << 20


class Gate:
    """Wait for a selected reply, then optionally deliver a held reply."""

    def __init__(self, request_id: int, hold: bool, diagnostic: Callable[[], str]):
        self.request_id = request_id
        self.frame: bytes | None = None
        self._hold = hold
        self._diagnostic = diagnostic
        self._ready = threading.Event()
        self._lock = threading.Lock()
        self._released = False
        self._deliver: Callable[[], None] | None = None
        self._error: str | None = None

    def wait(self, timeout: float = 5.0) -> Gate:
        """Return this gate once its reply is captured; fail on disconnect."""
        if not self._ready.wait(timeout):
            raise TimeoutError(
                f"reply {self.request_id} was not captured within {timeout}s; "
                + self._diagnostic()
            )
        with self._lock:
            error = self._error if self.frame is None else None
        if error is not None:
            raise ConnectionError(f"reply {self.request_id}: {error}; " + self._diagnostic())
        return self

    def release(self) -> None:
        """Deliver a held reply once. A dropped reply remains dropped."""
        with self._lock:
            if self._error is not None:
                raise ConnectionError(f"reply {self.request_id}: {self._error}")
            self._released = True
            deliver, self._deliver = self._deliver, None
        if deliver is not None:
            deliver()

    def _capture(self, frame: bytes, deliver: Callable[[], None]) -> None:
        with self._lock:
            self.frame = frame
            send_now = self._hold and self._released
            if self._hold and not self._released:
                self._deliver = deliver
            self._ready.set()
        if send_now:
            deliver()

    def _abort(self, reason: str) -> None:
        with self._lock:
            # Keep an already captured frame available for inspection, even
            # after the connection ends. An unreleased hold cannot be sent.
            self._error = reason
            self._deliver = None
            self._ready.set()


class ReplyProxy:
    """Forward a Unix RPC connection, with one-shot reply delivery rules."""

    def __init__(self, upstream_socket: str | Path, directory: str | Path):
        self.upstream_socket = str(upstream_socket)
        self.unix = str(Path(directory) / f"reply-{uuid.uuid4().hex[:10]}.sock")
        self._lock = threading.Lock()
        self._send_lock = threading.Lock()
        self._closed = threading.Event()
        self._sockets: list[socket.socket] = []
        self._threads: list[threading.Thread] = []
        self._rules: dict[int, Gate] = {}
        self._gates: list[Gate] = []
        self._errors: list[str] = []
        self._started = False

    @property
    def diagnostics(self) -> str:
        with self._lock:
            errors = list(self._errors)
            pending = list(self._rules)
        state = "closed" if self._closed.is_set() else "open"
        return "; ".join(errors) if errors else f"proxy {state}; pending replies: {pending}"

    def drop_reply(self, request_id: int) -> Gate:
        """Drop exactly this reply, preserving the daemon's real outcome."""
        return self._rule(request_id, hold=False)

    def hold_reply(self, request_id: int) -> Gate:
        """Hold exactly this reply until its gate is released."""
        return self._rule(request_id, hold=True)

    def _rule(self, request_id: int, hold: bool) -> Gate:
        if not isinstance(request_id, int) or isinstance(request_id, bool) or request_id < 1:
            raise ValueError("request_id must be a positive integer")
        with self._lock:
            if self._closed.is_set():
                raise RuntimeError("the proxy is closed")
            if request_id in self._rules:
                raise ValueError(f"a rule for reply {request_id} already exists")
            gate = Gate(request_id, hold, lambda: self.diagnostics)
            self._rules[request_id] = gate
            self._gates.append(gate)
        return gate

    def __enter__(self) -> ReplyProxy:
        if self._started or self._closed.is_set():
            raise RuntimeError("a reply proxy can only be started once")
        self._started = True
        Path(self.unix).parent.mkdir(parents=True, exist_ok=True)
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            listener.bind(self.unix)
            listener.listen(1)
            listener.settimeout(0.1)
            with self._lock:
                self._sockets.append(listener)
            self._start_thread(self._accept, listener, name="rpc-proxy-accept")
        except BaseException:
            listener.close()
            self.close()
            raise
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    def _start_thread(self, target, *args, name: str) -> None:
        thread = threading.Thread(target=target, args=args, name=name, daemon=True)
        # Starting under the lock prevents close() from joining a thread
        # that has been registered but has not yet been started.
        with self._lock:
            if self._closed.is_set():
                return
            self._threads.append(thread)
            thread.start()

    def _accept(self, listener: socket.socket) -> None:
        try:
            while not self._closed.is_set():
                try:
                    downstream, _ = listener.accept()
                    break
                except socket.timeout:
                    continue
            else:
                return
            upstream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            with self._lock:
                if self._closed.is_set():
                    downstream.close()
                    upstream.close()
                    return
                self._sockets.extend((downstream, upstream))
            # A broken upstream must not leave setup or cleanup waiting.
            upstream.settimeout(2.0)
            upstream.connect(self.upstream_socket)
            upstream.settimeout(None)
            listener.close()
            self._start_thread(self._forward, downstream, upstream, False, name="rpc-proxy-requests")
            self._start_thread(self._forward, upstream, downstream, True, name="rpc-proxy-replies")
        except Exception as error:
            self._failed("connect", error)

    @staticmethod
    def _read_exact(sock: socket.socket, size: int) -> bytes | None:
        data = bytearray()
        while len(data) < size:
            part = sock.recv(size - len(data))
            if not part:
                if data:
                    raise ConnectionError(f"connection ended after {len(data)} of {size} bytes")
                return None
            data.extend(part)
        return bytes(data)

    @classmethod
    def _read_frame(cls, sock: socket.socket) -> bytes | None:
        header = cls._read_exact(sock, 4)
        if header is None:
            return None
        size, = struct.unpack("<I", header)
        if size == 0 or size > MAX_FRAME_BYTES:
            raise ValueError(f"invalid RPC frame size: {size}")
        frame = cls._read_exact(sock, size)
        if frame is None:
            raise ConnectionError(f"connection ended before a {size}-byte frame")
        return frame

    @staticmethod
    def _reply_id(frame: bytes) -> int | None:
        # Generated public RPC bindings are the only protocol dependency.
        from Tsugumi.RPC.Message import Message
        from Tsugumi.RPC.Payload import Payload

        message = Message.GetRootAs(frame, 0)
        return message.RequestId() if message.PayloadType() == Payload.Reply else None

    def _send(self, downstream: socket.socket, frame: bytes) -> None:
        with self._send_lock:
            if self._closed.is_set():
                raise ConnectionError("the proxy connection is closed")
            downstream.sendall(struct.pack("<I", len(frame)) + frame)

    def _deliver_held(self, target: socket.socket, frame: bytes) -> None:
        try:
            self._send(target, frame)
        except Exception as error:
            self._failed("held reply delivery", error)
            raise

    def _forward(self, source: socket.socket, target: socket.socket, replies: bool) -> None:
        try:
            while not self._closed.is_set():
                frame = self._read_frame(source)
                if frame is None:
                    if replies:
                        self._shutdown("connection closed before reply")
                    else:
                        # Preserve a client's write-half close: requests
                        # already forwarded still belong to the daemon.
                        target.shutdown(socket.SHUT_WR)
                    return
                gate = None
                if replies:
                    request_id = self._reply_id(frame)
                    with self._lock:
                        gate = self._rules.pop(request_id, None)
                if gate is not None:
                    gate._capture(frame, lambda frame=frame: self._deliver_held(target, frame))
                elif replies:
                    self._send(target, frame)
                else:
                    target.sendall(struct.pack("<I", len(frame)) + frame)
        except Exception as error:
            self._failed("reply forwarding" if replies else "request forwarding", error)

    def _failed(self, operation: str, error: Exception) -> None:
        if not self._closed.is_set():
            with self._lock:
                self._errors.append(f"{operation}: {type(error).__name__}: {error}")
        self._shutdown(f"{operation} failed")

    def _shutdown(self, reason: str) -> None:
        self._closed.set()
        with self._lock:
            sockets, gates = list(self._sockets), list(self._gates)
        for gate in gates:
            gate._abort(reason)
        for sock in sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()

    def close(self) -> None:
        """Close all sockets and finish background threads within two seconds."""
        self._shutdown("proxy closed")
        with self._lock:
            threads = list(self._threads)
        deadline = time.monotonic() + 2.0
        for thread in threads:
            if thread is not threading.current_thread():
                thread.join(max(0.0, deadline - time.monotonic()))
        alive = [thread.name for thread in threads if thread.is_alive()]
        if alive:
            with self._lock:
                self._errors.append("threads did not stop: " + ", ".join(alive))
        Path(self.unix).unlink(missing_ok=True)
