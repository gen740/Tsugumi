"""Seeded malformed traffic must leave both public RPC transports available.

TSUGUMI_FUZZ_CASES controls the cases per transport (default 300). Every
mutation, payload and mask comes from the reported seed, so a failing case
can be reproduced without depending on operating-system random bytes.
"""

from __future__ import annotations

import errno
import os
import random
import socket
import struct
import time
import uuid

from harness.transport import hello_frame, port_of, unix_connection, unix_frame, ws_connect, ws_frame
from tsugumi_client import encode_request, make

CASES = int(os.environ.get("TSUGUMI_FUZZ_CASES", "300"))


def seeded_hello(rnd, token):
    return hello_frame(1, token, client_id=str(uuid.UUID(int=rnd.getrandbits(128))))


def requests(rnd):
    """Seed mutations with requests from the current public protocol."""
    kind, fields = rnd.choice([
        ("ProjectInspectRequest", {}),
        ("RunStatusRequest", {}),
        ("RunHistoryRequest", {"limit": 5}),
        ("ProjectFileListRequest", {"path": ".", "atRev": 0}),
    ])
    return encode_request(rnd.randrange(1, 1 << 30), kind, make(kind, **fields))


def mutate(rnd, data):
    result = bytearray(data)
    for _ in range(rnd.randint(1, 8)):
        operation = rnd.random()
        if operation < 0.5 and result:
            result[rnd.randrange(len(result))] = rnd.randrange(256)
        elif operation < 0.7 and result:
            index = rnd.randrange(len(result))
            del result[index:index + rnd.randint(1, 16)]
        elif operation < 0.85:
            index = rnd.randrange(len(result) + 1)
            result[index:index] = rnd.randbytes(rnd.randint(1, 32))
        elif result:
            index = rnd.randrange(len(result))
            result[index:index + 4] = struct.pack(
                "<I", rnd.choice([0, 0x7FFFFFFF, 0xFFFFFFFF, len(result), rnd.randrange(1 << 32)])
            )
    return bytes(result)


def any_ws_frame(rnd, payload):
    """Vary opcode, FIN, RSV, masking and advertised length independently."""
    first = rnd.choice([0x82, 0x82, 0x02, 0x80, 0x00, 0x81, 0x88, 0x89, 0x8A, 0x83, 0xC2, 0x8F])
    if rnd.random() >= 0.9:
        first = rnd.randrange(256)
    masked = rnd.random() < 0.9
    size = len(payload) if rnd.random() < 0.7 else rnd.choice([
        0, 125, 126, 65535, 65536, 1 << 31, (1 << 63) - 1, rnd.randrange(1 << 20),
    ])
    header = bytes([first])
    mask_flag = 0x80 if masked else 0
    if size < 126 and rnd.random() < 0.8:
        header += bytes([mask_flag | size])
    elif size < 65536 and rnd.random() < 0.8:
        header += bytes([mask_flag | 126]) + struct.pack(">H", size)
    else:
        header += bytes([mask_flag | 127]) + struct.pack(">Q", size)
    if masked:
        key = rnd.randbytes(4)
        header += key
        payload = bytes(value ^ key[index % 4] for index, value in enumerate(payload))
    return header + payload


def send_in_pieces(connection, data, rnd):
    """An invalid peer may be dropped at any point during its write."""
    pieces = []
    offset = 0
    while offset < len(data):
        end = offset + rnd.randint(1, len(data) - offset)
        pieces.append(data[offset:end])
        offset = end
    try:
        for piece in pieces:
            connection.sendall(piece)
    except (ConnectionError, socket.timeout):
        pass
    except OSError as error:
        if error.errno != errno.ENOTCONN:
            raise


def drain(connection):
    """Read bounded replies; an intentionally incomplete frame may remain idle."""
    deadline = time.monotonic() + 0.2
    try:
        while (remaining := deadline - time.monotonic()) > 0:
            connection.settimeout(remaining)
            if not connection.recv(65536):
                return
    except (ConnectionError, socket.timeout):
        pass
    except OSError as error:
        if error.errno != errno.ENOTCONN:
            raise


def still_serves(daemon, *, websocket=False):
    with daemon.connect(websocket=websocket) as client:
        assert client.call("ProjectInspectRequest", timeout=5).rev == 0


def test_unix_frames_of_any_shape(daemon):
    """Malformed Unix frames before and after Hello leave the public service available."""
    seed = 0x75A1
    rnd = random.Random(seed)
    assert CASES > 0, "TSUGUMI_FUZZ_CASES must exercise at least one case"
    for case in range(CASES):
        try:
            with unix_connection(daemon) as connection:
                connection.settimeout(2)
                shape = case % 4
                if shape == 0:
                    data = rnd.randbytes(rnd.randint(1, 4096))
                elif shape == 1:
                    body = rnd.randbytes(rnd.randint(0, 512))
                    size = rnd.choice([0, 1, len(body) + 1, 0xFFFFFFFF, rnd.randrange(1 << 32)])
                    data = struct.pack("<I", size) + body
                elif shape == 2:
                    data = unix_frame(mutate(rnd, seeded_hello(rnd, None)))
                else:
                    data = unix_frame(seeded_hello(rnd, None)) + b"".join(
                        unix_frame(mutate(rnd, requests(rnd))) for _ in range(rnd.randint(1, 6))
                    )
                send_in_pieces(connection, data, rnd)
                drain(connection)
            if case % 25 == 24:
                still_serves(daemon)
        except Exception as error:
            error.add_note(f"Unix fuzz seed={seed:#x}, case={case}")
            raise
    still_serves(daemon)


def test_websocket_frames_of_any_shape(daemon):
    """Malformed WebSocket frames leave both WebSocket and Unix RPC clients usable."""
    seed = 0x3B5
    rnd = random.Random(seed)
    port, token = port_of(daemon), daemon.ready["token"]
    assert CASES > 0, "TSUGUMI_FUZZ_CASES must exercise at least one case"
    for case in range(CASES):
        try:
            connection, status = ws_connect(port)
            with connection:
                assert status.split()[1] == "101"
                connection.settimeout(2)
                shape = case % 4
                if shape == 0:
                    data = rnd.randbytes(rnd.randint(1, 4096))
                elif shape == 1:
                    data = b"".join(any_ws_frame(rnd, rnd.choice([
                        seeded_hello(rnd, token), rnd.randbytes(rnd.randint(0, 300)),
                    ])) for _ in range(rnd.randint(1, 5)))
                elif shape == 2:
                    hello = seeded_hello(rnd, token)
                    cut = rnd.randint(1, len(hello) - 1)
                    parts = [any_ws_frame(rnd, hello[:cut]),
                             any_ws_frame(rnd, rnd.randbytes(rnd.randint(0, 200))),
                             any_ws_frame(rnd, hello[cut:])]
                    rnd.shuffle(parts)
                    data = b"".join(parts)
                else:
                    data = ws_frame(seeded_hello(rnd, token), mask=rnd.randbytes(4)) + b"".join(
                        ws_frame(mutate(rnd, requests(rnd)), mask=rnd.randbytes(4))
                        for _ in range(rnd.randint(1, 6))
                    )
                send_in_pieces(connection, data, rnd)
                drain(connection)
            if case % 25 == 24:
                still_serves(daemon)
                still_serves(daemon, websocket=True)
        except Exception as error:
            error.add_note(f"WebSocket fuzz seed={seed:#x}, case={case}")
            raise
    still_serves(daemon)
    still_serves(daemon, websocket=True)
