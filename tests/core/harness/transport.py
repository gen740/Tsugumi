"""Raw public-RPC transports shared by framing and malformed-traffic scenarios."""

from __future__ import annotations

import base64
from contextlib import contextmanager
import os
import socket
import struct

from tsugumi_client import encode_request, make, new_id
from Tsugumi.RPC.Message import Message


def hello_frame(request_id: int, token: str | None, *, client_id=None) -> bytes:
    fields = {"protocol": 1, "clientId": client_id or new_id()}
    if token is not None:
        fields["token"] = token
    return encode_request(request_id, "SessionHelloRequest", make("SessionHelloRequest", **fields))


@contextmanager
def unix_connection(daemon):
    """Own a raw connection while testing framing instead of the client codec."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(5)
        connection.connect(daemon.unix)
        yield connection


def exact(connection, size):
    result = bytearray()
    while len(result) < size:
        chunk = connection.recv(size - len(result))
        if not chunk:
            if result:
                raise ConnectionError(f"RPC frame ended after {len(result)} of {size} bytes")
            return None
        result.extend(chunk)
    return bytes(result)


def unix_frame(payload):
    return struct.pack("<I", len(payload)) + payload


def unix_read(connection):
    header = exact(connection, 4)
    if header is None:
        return None
    size, = struct.unpack("<I", header)
    assert 0 < size <= 16 << 20
    return exact(connection, size)


def ws_connect(port, origin="http://rpc.test", protocol="tsugumi.rpc.v1"):
    connection = socket.create_connection(("127.0.0.1", port), timeout=5)
    try:
        key = base64.b64encode(os.urandom(16)).decode()
        lines = ["GET /rpc HTTP/1.1", f"Host: 127.0.0.1:{port}", "Upgrade: websocket", "Connection: Upgrade",
                 f"Sec-WebSocket-Key: {key}", "Sec-WebSocket-Version: 13"]
        if protocol:
            lines.append(f"Sec-WebSocket-Protocol: {protocol}")
        if origin:
            lines.append(f"Origin: {origin}")
        connection.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        response = bytearray()
        while not response.endswith(b"\r\n\r\n"):
            chunk = connection.recv(1)
            if not chunk:
                break
            response.extend(chunk)
            assert len(response) <= 65536, "WebSocket handshake headers exceeded 64 KiB"
        return connection, bytes(response).split(b"\r\n", 1)[0].decode()
    except BaseException:
        connection.close()
        raise


def ws_frame(payload, opcode=0x2, fin=True, *, mask=None):
    mask = os.urandom(4) if mask is None else mask
    head = bytes([(0x80 if fin else 0) | opcode])
    size = len(payload)
    if size < 126:
        head += bytes([0x80 | size])
    elif size < 65536:
        head += bytes([0x80 | 126]) + struct.pack(">H", size)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", size)
    return head + mask + bytes(value ^ mask[index % 4] for index, value in enumerate(payload))


def ws_packet(connection):
    header = exact(connection, 2)
    if header is None:
        return None, None
    assert not header[1] & 0x80, "server frames must not be masked"
    size = header[1] & 0x7F
    if size == 126:
        size, = struct.unpack(">H", exact(connection, 2))
    elif size == 127:
        size, = struct.unpack(">Q", exact(connection, 8))
    assert size <= 16 << 20
    return header[0] & 0x0F, exact(connection, size)


def ws_read(connection):
    opcode, payload = ws_packet(connection)
    return None if opcode in (None, 0x8) else payload


def reply_kind(data):
    return Message.GetRootAs(data, 0).PayloadType()


def port_of(daemon):
    return int(daemon.ready["endpoint"].rsplit(":", 1)[1].split("/")[0])
