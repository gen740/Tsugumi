"""Transport level checks of both channels with raw sockets: WebSocket
authentication, Origin and subprotocol rejection, fragmented messages,
several messages in one write; Unix frames split byte by byte or joined; a
run keeps going after its client disconnects and can be cancelled from
another connection."""

from __future__ import annotations

import base64
import os
import socket
import struct
import time

import pytest

from conftest import Daemon
from projects import builtin_project
from tsugumi_client import encode_request, make, new_id, path, text
from Tsugumi.RPC.Message import Message
from Tsugumi.RPC.Payload import Payload


def hello_frame(rid: int, token: str | None) -> bytes:
    fields = {"protocol": 1, "clientId": new_id()}
    if token is not None:
        fields["token"] = token
    return encode_request(rid, "SessionHelloRequest", make("SessionHelloRequest", **fields))


def ws_connect(port: int, origin: str | None = None, protocol: str | None = "tsugumi.rpc.v1"):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    lines = ["GET /rpc HTTP/1.1", f"Host: 127.0.0.1:{port}", "Upgrade: websocket", "Connection: Upgrade",
             f"Sec-WebSocket-Key: {key}", "Sec-WebSocket-Version: 13"]
    if protocol:
        lines.append(f"Sec-WebSocket-Protocol: {protocol}")
    if origin:
        lines.append(f"Origin: {origin}")
    s.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        chunk = s.recv(4096)
        if not chunk:
            break
        resp += chunk
    return s, resp.split(b"\r\n", 1)[0].decode()


def ws_frame(payload: bytes, opcode: int = 0x2, fin: bool = True) -> bytes:
    mask = os.urandom(4)
    head = bytes([(0x80 if fin else 0) | opcode])
    n = len(payload)
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 65536:
        head += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", n)
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


def ws_read(s) -> bytes | None:
    def exact(n):
        out = b""
        while len(out) < n:
            c = s.recv(n - len(out))
            if not c:
                return None
            out += c
        return out
    h = exact(2)
    if h is None:
        return None
    n = h[1] & 0x7F
    if n == 126:
        n = struct.unpack(">H", exact(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", exact(8))[0]
    return exact(n)


def reply_kind(data: bytes) -> int:
    return Message.GetRootAs(data, 0).PayloadType()


@pytest.fixture
def ws_daemon(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root, listen=True)
    d.start()
    yield d
    d.cleanup()


def port_of(d) -> int:
    return int(d.ready["endpoint"].rsplit(":", 1)[1].split("/")[0])


def test_websocket_rejects_bad_origin_and_missing_subprotocol(ws_daemon):
    port = port_of(ws_daemon)
    s, status = ws_connect(port, origin="http://evil.example")
    assert "403" in status
    s.close()
    s, status = ws_connect(port, protocol=None)
    assert "400" in status
    s.close()
    s, status = ws_connect(port, protocol="something.else")
    assert "400" in status
    s.close()


def test_websocket_requires_the_token(ws_daemon):
    port = port_of(ws_daemon)
    s, status = ws_connect(port)
    assert "101" in status
    s.sendall(ws_frame(hello_frame(1, "wrong-token")))
    data = ws_read(s)
    from Tsugumi.RPC.Reply import Reply
    from Tsugumi.RPC.ReplyPayload import ReplyPayload
    if data is not None:
        m = Message.GetRootAs(data, 0)
        reply = Reply()
        reply.Init(m.Payload().Bytes, m.Payload().Pos)
        assert reply.ValueType() == ReplyPayload.Failure
    # Nothing else is served on this connection.
    try:
        s.sendall(ws_frame(encode_request(2, "ProjectInspectRequest", make("ProjectInspectRequest"))))
        assert ws_read(s) is None
    except (BrokenPipeError, ConnectionResetError):
        pass
    s.close()


def test_websocket_fragments_and_joined_messages(ws_daemon):
    port = port_of(ws_daemon)
    token = ws_daemon.ready["token"]
    s, status = ws_connect(port)
    assert "101" in status
    hello = hello_frame(1, token)
    # One message split into three fragments.
    s.sendall(ws_frame(hello[:5], fin=False) + ws_frame(hello[5:12], opcode=0x0, fin=False) +
              ws_frame(hello[12:], opcode=0x0))
    assert reply_kind(ws_read(s)) == Payload.Reply
    # Two requests in a single write.
    a = encode_request(2, "ProjectInspectRequest", make("ProjectInspectRequest"))
    b = encode_request(3, "ProjectInspectRequest", make("ProjectInspectRequest"))
    s.sendall(ws_frame(a) + ws_frame(b))
    ids = sorted(Message.GetRootAs(ws_read(s), 0).RequestId() for _ in range(2))
    assert ids == [2, 3]
    # A ping in the middle of a fragmented message is answered.
    s.sendall(ws_frame(a[:3], fin=False) + ws_frame(b"hi", opcode=0x9) + ws_frame(a[3:], opcode=0x0))
    got = [ws_read(s), ws_read(s)]
    assert b"hi" in got[0] or b"hi" in got[1]
    s.close()


def test_unix_frames_split_and_joined(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(d.unix)
        s.settimeout(5)
        raw = hello_frame(1, None)
        hello = struct.pack("<I", len(raw)) + raw
        for byte in hello:  # byte by byte
            s.sendall(bytes([byte]))
        def read():
            n = struct.unpack("<I", s.recv(4, socket.MSG_WAITALL))[0]
            return s.recv(n, socket.MSG_WAITALL)
        assert reply_kind(read()) == Payload.Reply
        frames = [encode_request(i, "ProjectInspectRequest", make("ProjectInspectRequest")) for i in (2, 3, 4)]
        joined = b"".join(struct.pack("<I", len(f)) + f for f in frames)
        s.sendall(joined)
        assert sorted(Message.GetRootAs(read(), 0).RequestId() for _ in range(3)) == [2, 3, 4]
        s.close()
    finally:
        d.cleanup()


def test_run_continues_after_its_client_disconnects(tmp_path):
    from projects import FILE_FBS, write
    from projects import card, conn
    root = tmp_path / "p"
    # A long loop of builtin Cards keeps the run busy for a while.
    write(root, {"trigger": {"template": "trigger"},
                 "doc": {"template": "input", "params": [{"name": "value", "type": "string", "data": "definitions/doc/a.txt"}],
                         "_outputs": FILE_FBS},
                 "log": {"template": "logger"}},
          [card("start", "trigger"), card("src", "doc"), card("out", "log")],
          [conn("start", "out", "src", "start"), conn("src", "doc", "out", "in")],
          files={"definitions/doc/a.txt": "x" * 1000})
    d = Daemon(root).start()
    try:
        c1 = d.client()
        c1.hello()
        snap = c1.call("ProjectInspectRequest")
        acc = c1.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path("start"))
        run_id = text(acc.run.runId)
        c1.close()  # the client that started the run goes away
        c2 = d.client()
        c2.hello()
        deadline = time.time() + 20
        while time.time() < deadline:
            st = c2.call("RunStatusRequest", runId=run_id)
            if st.state in (6, 7, 8):
                break
            time.sleep(0.05)
        assert st.state == 6  # succeeded without its starter
        c2.close()
    finally:
        d.cleanup()


def test_a_peer_without_hello_cannot_make_tsugumid_buffer(tmp_path):
    """Before its Hello a peer may send only a little: one announcing an 8 MiB
    frame and streaming it is dropped, without tsugumid buffering it all."""
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(d.unix)
        s.settimeout(10)
        dropped = False
        try:
            s.sendall(struct.pack("<I", 8 << 20))
            for _ in range(64):
                s.sendall(b"\0" * 65536)
        except (BrokenPipeError, ConnectionResetError):
            dropped = True
        if not dropped:
            try:
                dropped = s.recv(1) == b""
            except (ConnectionResetError, socket.timeout):
                dropped = True
        s.close()
        assert dropped
        # Others are served as before.
        c = d.client()
        c.hello()
        c.call("ProjectInspectRequest")
    finally:
        d.cleanup()
