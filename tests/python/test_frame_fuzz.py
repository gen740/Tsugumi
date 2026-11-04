"""Fuzzing the two channels' frame parsing with a fixed seed: random bytes,
WebSocket frames with any header bits, lengths and masks, Unix frames with any
length prefix, and well-formed requests with bytes flipped, before and after
Hello. tsugumid drops a bad connection and keeps serving everyone else; it
never exits, hangs or answers a later client wrongly.

TSUGUMI_FUZZ_CASES sets how many cases each channel gets (default 300)."""

from __future__ import annotations

import os
import random
import socket
import struct

import pytest

from conftest import Daemon
from projects import builtin_project
from test_transport import hello_frame, port_of, reply_kind, ws_connect, ws_frame, ws_read
from tsugumi_client import encode_request, make
from Tsugumi.RPC.Payload import Payload

CASES = int(os.environ.get("TSUGUMI_FUZZ_CASES", "300"))


def requests(rnd: random.Random) -> bytes:
    """A well-formed request of a few kinds, the seed of a mutation."""
    kind = rnd.choice(["ProjectInspectRequest", "RunStatusRequest", "DocGetRequest", "RunHistoryListRequest"])
    try:
        return encode_request(rnd.randrange(1, 1 << 30), kind, make(kind))
    except Exception:  # a kind needs fields: inspect is enough
        return encode_request(rnd.randrange(1, 1 << 30), "ProjectInspectRequest", make("ProjectInspectRequest"))


def mutate(rnd: random.Random, data: bytes) -> bytes:
    b = bytearray(data)
    for _ in range(rnd.randint(1, 8)):
        op = rnd.random()
        if op < 0.5 and b:
            b[rnd.randrange(len(b))] = rnd.randrange(256)
        elif op < 0.7 and b:
            i = rnd.randrange(len(b))
            del b[i:i + rnd.randint(1, 16)]
        elif op < 0.85:
            b[rnd.randrange(len(b) + 1):0] = os.urandom(rnd.randint(1, 32))
        elif b:
            i = rnd.randrange(len(b))
            b[i:i + 4] = struct.pack("<I", rnd.choice([0, 0x7FFFFFFF, 0xFFFFFFFF, len(b), rnd.randrange(1 << 32)]))
    return bytes(b)


def any_ws_frame(rnd: random.Random, payload: bytes) -> bytes:
    """A frame whose header may say anything: opcode, FIN, RSV, mask bit and
    a length that may not match what follows."""
    b0 = rnd.choice([0x82, 0x82, 0x02, 0x80, 0x00, 0x81, 0x88, 0x89, 0x8A, 0x83, 0xC2, 0x8F]) if rnd.random() < 0.9 else rnd.randrange(256)
    masked = rnd.random() < 0.9
    n = len(payload) if rnd.random() < 0.7 else rnd.choice([0, 125, 126, 65535, 65536, 1 << 31, (1 << 63) - 1, rnd.randrange(1 << 20)])
    head = bytes([b0])
    m = 0x80 if masked else 0
    if n < 126 and rnd.random() < 0.8:
        head += bytes([m | n])
    elif n < 65536 and rnd.random() < 0.8:
        head += bytes([m | 126]) + struct.pack(">H", n)
    else:
        head += bytes([m | 127]) + struct.pack(">Q", n)
    if masked:
        key = os.urandom(4)
        head += key
        payload = bytes(c ^ key[i % 4] for i, c in enumerate(payload))
    return head + payload


def send_all(s: socket.socket, data: bytes, rnd: random.Random) -> None:
    """Sends in random pieces; a drop by tsugumid ends it quietly."""
    try:
        i = 0
        while i < len(data):
            j = i + rnd.randint(1, max(1, len(data) - i))
            s.sendall(data[i:j])
            i = j
    except (ConnectionError, socket.timeout, OSError):
        pass


def drain(s: socket.socket) -> None:
    """Reads what tsugumid answers until it closes or goes quiet."""
    s.settimeout(0.2)
    try:
        while s.recv(65536):
            pass
    except (ConnectionError, socket.timeout, OSError):
        pass
    s.close()


def unix_frame(payload: bytes) -> bytes:
    return struct.pack("<I", len(payload)) + payload


def still_serves(d: Daemon) -> None:
    assert d.proc.poll() is None, d.log.read_text()[-2000:]
    c = d.client()
    c.hello()
    c.call("ProjectInspectRequest")
    c.close()


@pytest.fixture
def fuzz_daemon(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root, listen=True).start()
    yield d
    d.cleanup()


def test_unix_frames_of_any_shape(fuzz_daemon):
    d, rnd = fuzz_daemon, random.Random(0x75A1)
    for case in range(CASES):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(d.unix)
        s.settimeout(2)
        shape = case % 4
        if shape == 0:  # bytes of anything, before Hello
            data = os.urandom(rnd.randint(1, 4096))
        elif shape == 1:  # a length prefix that lies
            body = os.urandom(rnd.randint(0, 512))
            data = struct.pack("<I", rnd.choice([0, 1, len(body) + 1, 0xFFFFFFFF, rnd.randrange(1 << 32)])) + body
        elif shape == 2:  # a Hello with bytes flipped
            data = unix_frame(mutate(rnd, hello_frame(1, None)))
        else:  # after a good Hello, requests with bytes flipped
            data = unix_frame(hello_frame(1, None)) + b"".join(unix_frame(mutate(rnd, requests(rnd))) for _ in range(rnd.randint(1, 6)))
        send_all(s, data, rnd)
        drain(s)
        if case % 25 == 24:
            still_serves(d)
    still_serves(d)


def test_websocket_frames_of_any_shape(fuzz_daemon):
    d, rnd = fuzz_daemon, random.Random(0x3B5)
    port, token = port_of(d), d.ready["token"]
    for case in range(CASES):
        s, status = ws_connect(port)
        assert "101" in status
        s.settimeout(2)
        shape = case % 4
        if shape == 0:  # anything after the handshake
            data = os.urandom(rnd.randint(1, 4096))
        elif shape == 1:  # frames of any header around a Hello
            data = b"".join(any_ws_frame(rnd, rnd.choice([hello_frame(1, token), os.urandom(rnd.randint(0, 300))]))
                            for _ in range(rnd.randint(1, 5)))
        elif shape == 2:  # fragments, control frames and continuations out of order
            hello = hello_frame(1, token)
            cut = rnd.randint(1, len(hello) - 1)
            parts = [any_ws_frame(rnd, hello[:cut]), any_ws_frame(rnd, os.urandom(rnd.randint(0, 200))), any_ws_frame(rnd, hello[cut:])]
            rnd.shuffle(parts)
            data = b"".join(parts)
        else:  # after a good Hello, requests with bytes flipped in good frames
            data = ws_frame(hello_frame(1, token)) + b"".join(ws_frame(mutate(rnd, requests(rnd))) for _ in range(rnd.randint(1, 6)))
        send_all(s, data, rnd)
        drain(s)
        if case % 25 == 24:
            still_serves(d)
    still_serves(d)
    # and a WebSocket client is served as before
    s, status = ws_connect(port)
    assert "101" in status
    s.sendall(ws_frame(hello_frame(1, token)))
    assert reply_kind(ws_read(s)) == Payload.Reply
    s.close()
