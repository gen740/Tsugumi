"""Durable operation identities and ordered outcomes through the real socket."""
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import threading

import pytest

from tsugumi_client import RpcFailure, change, make, new_id, text
from Tsugumi.Data.OperationState import OperationState
from Tsugumi.Data.Failure import FailureT
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from test_rpc_operations import save_request, project_name


def operation(client, semantic="rename"):
    lane = client.call("WriterOpenRequest")
    identity = make("OperationIdentity", lane=text(lane.lane), sequence=1)
    return make("OperationSubmission", identity=identity,
                semanticDigest=hashlib.sha256(semantic.encode()).hexdigest())


def test_identical_concurrent_id_returns_the_original_outcome(daemon):
    clients = [daemon.connect(), daemon.connect()]
    request = save_request(clients[0].call("ProjectInspectRequest"), "one-change")
    request["operation"] = operation(clients[0])
    barrier = threading.Barrier(2)

    def submit(client):
        barrier.wait(timeout=5)
        return client.call("ProjectSaveRequest", **request)

    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(submit, clients))
    assert sorted(result.replayed for result in results) == [False, True]
    assert {result.cursor for result in results} == {1}
    assert all(result.delta is None for result in results)
    assert project_name(clients[0]) == (1, "one-change")
    status = clients[0].call("WriterStatusRequest", operation=request["operation"])
    assert status.state == OperationState.completed
    assert status.receipt.cursor == 1
    request["operation"].semanticDigest = hashlib.sha256(b"different").hexdigest()
    with pytest.raises(RpcFailure, match="operation_conflict"):
        clients[0].call("ProjectSaveRequest", **request)


def test_retirement_is_durable_and_apply_never_creates_a_lane(daemon):
    client = daemon.connect()
    request = save_request(client.call("ProjectInspectRequest"), "retired")
    request["operation"] = operation(client)
    result = client.call("ProjectSaveRequest", **request)
    with pytest.raises(RpcFailure, match="operation_conflict"):
        client.call("WriterAcknowledgeRequest", identity=request["operation"].identity,
                    incorporatedCursor=result.cursor - 1)
    client.call("WriterAcknowledgeRequest", identity=request["operation"].identity,
                incorporatedCursor=result.cursor, retire=True)
    daemon.restart()
    client = daemon.connect()
    with pytest.raises(RpcFailure, match="operation_retired"):
        client.call("ProjectSaveRequest", **request)
    request["operation"].identity.lane = new_id()
    with pytest.raises(RpcFailure, match="operation_retired"):
        client.call("ProjectSaveRequest", **request)
    assert project_name(client) == (1, "retired")


@pytest.mark.parametrize("fault", ["save_after_prepare", "save_during_apply", "save_before_commit", "after_operation_commit"])
def test_recovery_finishes_the_exact_plan_and_replays_the_lane(core_factory, fault):
    daemon = core_factory(fault=fault)
    client = daemon.connect()
    request = save_request(client.call("ProjectInspectRequest"), "recovered")
    request["operation"] = operation(client)
    sent = client.send("ProjectSaveRequest", make("ProjectSaveRequest", **request))
    with pytest.raises((ConnectionError, TimeoutError)):
        client.wait_reply(sent, timeout=5)
    daemon.restart()
    client = daemon.connect()
    status = client.call("WriterStatusRequest", operation=request["operation"])
    assert status.state == OperationState.completed
    assert status.receipt.cursor == 1
    # Replay is independent of the expanded concrete plan/revision.
    request["baseRev"] = 9999
    request["changes"] = []
    replay = client.call("ProjectSaveRequest", **request)
    assert replay.replayed and replay.rev == 1 and replay.cursor == 1
    assert project_name(client) == (1, "recovered")


def test_stream_orders_legacy_writes_noops_and_refusals_without_fake_revisions(daemon):
    client = daemon.connect()
    opened = client.call("ProjectOpenRequest", operationStream=True)
    assert opened.snapshot.operationCursor == 0
    client.call("ProjectSaveRequest", **save_request(opened.snapshot, "legacy"))
    submission = operation(client)
    noop = client.call("ProjectSaveRequest", baseRev=1, opId=new_id(), origin="agent",
                       changes=[], operation=submission)
    assert noop.rev == 1 and noop.cursor == 2
    client.call("WriterAcknowledgeRequest", identity=submission.identity,
                incorporatedCursor=noop.cursor)
    submission.identity.sequence = 2
    failure = FailureT(); failure.kind = 17; failure.diagnostic = "target changed"; failure.args = []
    with pytest.raises(RpcFailure, match="validation_failed"):
        client.call("OperationRefuseRequest", operation=submission, opId=new_id(),
                    failure=failure)
    events = [client.wait_notification(lambda n: n.valueType == NotificationPayload.ProjectOperationEvent,
                                      timeout=5).value for _ in range(3)]
    assert [event.receipt.cursor for event in events] == [1, 2, 3]
    assert events[0].delta.rev == 1
    assert events[1].delta is None and events[2].delta is None
    assert client.call("ProjectInspectRequest").rev == 1
    assert client.call("ProjectInspectRequest").operationCursor == 3


def test_pending_digest_survives_restart_and_rejects_a_different_payload(daemon):
    client = daemon.connect()
    submitted = operation(client, "waiting for approval")
    client.call("WriterBeginRequest", operation=submitted)
    assert client.call("WriterStatusRequest", operation=submitted).state == OperationState.pending
    daemon.restart()
    client = daemon.connect()
    assert client.call("WriterStatusRequest", operation=submitted).state == OperationState.pending
    different = make("OperationSubmission", identity=submitted.identity,
                     semanticDigest=hashlib.sha256(b"different while pending").hexdigest())
    with pytest.raises(RpcFailure, match="operation_conflict"):
        client.call("WriterBeginRequest", operation=different)
    assert client.call("ProjectInspectRequest").operationCursor == 0


def test_crash_before_noop_receipt_keeps_the_intent_pending(core_factory):
    daemon = core_factory(fault="before_receipt")
    client = daemon.connect()
    submitted = operation(client, "noop")
    client.call("WriterBeginRequest", operation=submitted)
    request = dict(baseRev=0, opId=new_id(), origin="agent", changes=[], operation=submitted)
    sent = client.send("ProjectSaveRequest", make("ProjectSaveRequest", **request))
    with pytest.raises((ConnectionError, TimeoutError)):
        client.wait_reply(sent, timeout=5)
    daemon.restart()
    client = daemon.connect()
    assert client.call("WriterStatusRequest", operation=submitted).state == OperationState.pending
    result = client.call("ProjectSaveRequest", **request)
    assert result.rev == 0 and result.cursor == 1
