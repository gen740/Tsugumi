"""Wire-level behavior of the real core's Unix and WebSocket RPC endpoints."""

from __future__ import annotations

import errno
import json
import socket
import struct

import flatbuffers
import pytest

from harness.fixtures import worker_project
from harness.scenario import Gate, run, wait_run
from harness.transport import (
    hello_frame, port_of, reply_kind, unix_connection, unix_frame, unix_read,
    ws_connect, ws_frame, ws_packet, ws_read,
)
from tsugumi_client import change, decode_message, encode_request, make, new_id, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC import Message as MessageBuilder, Request
from Tsugumi.RPC.Message import Message
from Tsugumi.RPC.Payload import Payload
from Tsugumi.RPC.ReplyPayload import ReplyPayload


@pytest.mark.parametrize("origin,protocol,status_code", [
    ("http://evil.example", "tsugumi.rpc.v1", 403),
    (None, "tsugumi.rpc.v1", 403),
    ("http://rpc.test", None, 400),
    ("http://rpc.test", "something.else", 400),
], ids=["untrusted-origin", "missing-origin", "missing-subprotocol", "wrong-subprotocol"])
def test_websocket_rejects_untrusted_handshakes(daemon, origin, protocol, status_code):
    """WebSocket upgrades require an allowed origin and the RPC subprotocol."""
    connection, status = ws_connect(port_of(daemon), origin, protocol)
    with connection:
        assert status.split()[1] == str(status_code)


@pytest.mark.parametrize("token", [None, "wrong-token"], ids=["missing-token", "wrong-token"])
def test_websocket_requires_the_token(daemon, token):
    """A WebSocket peer without the session token cannot inspect the project."""
    connection, status = ws_connect(port_of(daemon))
    with connection:
        assert status.split()[1] == "101"
        connection.sendall(ws_frame(hello_frame(1, token)))
        data = ws_read(connection)
        if data is not None:
            response = decode_message(data)
            assert response.payloadType == Payload.Reply
            assert response.payload.valueType == ReplyPayload.Failure
            assert response.payload.value.kind == FailureKind.unauthenticated
        try:
            connection.sendall(ws_frame(encode_request(2, "ProjectInspectRequest", make("ProjectInspectRequest"))))
            assert ws_read(connection) is None
        except ConnectionError:
            pass
    assert daemon.connect(websocket=True).call("ProjectInspectRequest").rev == 0


def test_websocket_fragments_and_joined_messages(daemon):
    """WebSocket RPC accepts fragmented and joined messages, including an interleaved ping."""
    connection, status = ws_connect(port_of(daemon))
    with connection:
        assert status.split()[1] == "101"
        hello = hello_frame(1, daemon.ready["token"])
        connection.sendall(ws_frame(hello[:5], fin=False) + ws_frame(hello[5:12], opcode=0x0, fin=False) +
                           ws_frame(hello[12:], opcode=0x0))
        assert reply_kind(ws_read(connection)) == Payload.Reply
        requests = [encode_request(index, "ProjectInspectRequest", make("ProjectInspectRequest")) for index in (2, 3)]
        connection.sendall(b"".join(ws_frame(request) for request in requests))
        replies = [decode_message(ws_read(connection)) for _ in requests]
        assert sorted(reply.requestId for reply in replies) == [2, 3]
        assert all(reply.payload.valueType == ReplyPayload.ProjectSnapshot for reply in replies)
        request = encode_request(4, "ProjectInspectRequest", make("ProjectInspectRequest"))
        connection.sendall(ws_frame(request[:3], fin=False) + ws_frame(b"hi", opcode=0x9) +
                           ws_frame(request[3:], opcode=0x0))
        packets = [ws_packet(connection), ws_packet(connection)]
        assert (0xA, b"hi") in packets
        reply = next(payload for opcode, payload in packets if opcode == 0x2)
        assert Message.GetRootAs(reply).RequestId() == 4


def test_unix_frames_split_and_joined(daemon):
    """Unix RPC accepts bytewise frames and several frames sent in one write."""
    with unix_connection(daemon) as connection:
        for value in unix_frame(hello_frame(1, None)):
            connection.sendall(bytes([value]))
        assert reply_kind(unix_read(connection)) == Payload.Reply
        requests = [encode_request(index, "ProjectInspectRequest", make("ProjectInspectRequest")) for index in (2, 3, 4)]
        connection.sendall(b"".join(unix_frame(request) for request in requests))
        replies = [decode_message(unix_read(connection)) for _ in requests]
        assert sorted(reply.requestId for reply in replies) == [2, 3, 4]
        assert all(reply.payload.valueType == ReplyPayload.ProjectSnapshot for reply in replies)


@pytest.mark.parametrize("cancel", [False, True], ids=["finishes-without-starter", "cancelled-by-another-client"])
def test_another_client_can_follow_and_cancel_a_disconnected_run(project, core_factory, tmp_path, cancel):
    """Disconnecting a run's starter leaves the run available to another client."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    starter = daemon.connect()
    gate = Gate(tmp_path, "disconnect")
    worker_project(starter, project, tmp_path, [gate.action(),
                   {"op": "output", "port": "out", "type": "string", "value": "finished"}])
    run_id = run(starter)
    gate.wait()
    starter.close()
    observer = daemon.connect()
    running = observer.call("RunStatusRequest", runId=run_id)
    assert running.state not in (RunState.succeeded, RunState.failed, RunState.cancelled)
    if cancel:
        observer.call("RunCancelRequest", runId=run_id)
    else:
        gate.release()
    assert wait_run(observer, run_id).state == (RunState.cancelled if cancel else RunState.succeeded)


def test_unknown_request_union_closes_only_the_offending_connection(daemon):
    """An unknown request type closes its connection while other clients remain usable."""
    builder = flatbuffers.Builder(64)
    Request.Start(builder)
    Request.AddValueType(builder, 200)
    request = Request.End(builder)
    MessageBuilder.Start(builder)
    MessageBuilder.AddRequestId(builder, 9)
    MessageBuilder.AddPayloadType(builder, Payload.Request)
    MessageBuilder.AddPayload(builder, request)
    builder.Finish(MessageBuilder.End(builder), file_identifier=b"TSG1")
    with unix_connection(daemon) as connection:
        connection.sendall(unix_frame(hello_frame(1, None)))
        assert reply_kind(unix_read(connection)) == Payload.Reply
        connection.sendall(unix_frame(bytes(builder.Output())))
        try:
            assert connection.recv(1) == b""
        except ConnectionError:
            pass
    assert daemon.connect().call("ProjectInspectRequest").rev == 0


@pytest.mark.parametrize("size,authenticated", [(0, False), (17 << 20, True)],
                         ids=["empty-frame", "oversized-authenticated-frame"])
def test_invalid_frame_size_is_rejected_without_waiting_for_its_body(daemon, size, authenticated):
    """Invalid frame lengths close the peer immediately and leave other clients available."""
    with unix_connection(daemon) as connection:
        if authenticated:
            connection.sendall(unix_frame(hello_frame(1, None)))
            assert reply_kind(unix_read(connection)) == Payload.Reply
        connection.sendall(struct.pack("<I", size))
        try:
            assert connection.recv(1) == b""
        except ConnectionError:
            pass
    assert daemon.connect().call("ProjectInspectRequest").rev == 0


def test_a_peer_without_hello_cannot_stream_a_large_frame(daemon):
    """A peer streaming a large frame before Hello is dropped before its full body arrives."""
    with unix_connection(daemon) as connection:
        try:
            connection.sendall(struct.pack("<I", 8 << 20))
            for _ in range(64):
                connection.sendall(b"\0" * 65536)
            assert connection.recv(1) == b""
        except ConnectionError:
            pass
        except OSError as error:
            if error.errno != errno.ENOTCONN:
                raise
    assert daemon.connect().call("ProjectInspectRequest").rev == 0


def test_slow_subscriber_is_dropped_without_stopping_other_clients(daemon):
    """A subscriber that stops reading is disconnected while another client keeps saving."""
    with unix_connection(daemon) as stalled:
        stalled.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        stalled.sendall(unix_frame(hello_frame(1, None)))
        assert reply_kind(unix_read(stalled)) == Payload.Reply
        stalled.sendall(unix_frame(encode_request(2, "ProjectOpenRequest", make("ProjectOpenRequest"))))
        assert reply_kind(unix_read(stalled)) == Payload.Reply
        writer = daemon.connect()
        snapshot = writer.call("ProjectInspectRequest")
        document = json.loads(text(snapshot.projectJson))
        revision = snapshot.rev
        for index in range(300):
            document["name"] = "x" * 200000 + str(index)
            revision = writer.call("ProjectSaveRequest", timeout=5, baseRev=revision, opId=new_id(), origin="ui:test",
                                   changes=[change("SetProject", json=json.dumps(document))]).rev
        assert revision == snapshot.rev + 300
        received = 0
        try:
            while chunk := stalled.recv(1 << 20):
                received += len(chunk)
        except ConnectionError:
            pass
        assert received < 300 * 200000
        assert writer.call("ProjectInspectRequest").rev == revision
