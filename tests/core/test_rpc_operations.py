"""Save, recovery and subscription contracts through real public RPC."""

from concurrent.futures import ThreadPoolExecutor
import json
import threading

import pytest

from harness.proxy import ReplyProxy
from tsugumi_client import RpcFailure, change, make, new_id, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.OperationState import OperationState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.ReceiptValue import ReceiptValue


def save_request(snapshot, name, op_id=None):
    document = json.loads(text(snapshot.projectJson))
    document["name"] = name
    return dict(baseRev=snapshot.rev, opId=op_id or new_id(), origin="test",
                changes=[change("SetProject", json=json.dumps(document))])


def project_name(client):
    snapshot = client.call("ProjectInspectRequest")
    return snapshot.rev, json.loads(text(snapshot.projectJson))["name"]


def test_concurrent_saves_commit_one_winner(daemon):
    """Only one writer can save against the same project revision."""
    clients = [daemon.connect() for _ in range(6)]
    snapshot = clients[0].call("ProjectInspectRequest")
    barrier = threading.Barrier(len(clients))

    def save(index):
        barrier.wait(timeout=5)
        name = f"writer-{index}"
        try:
            result = clients[index].call("ProjectSaveRequest", timeout=5,
                                         **save_request(snapshot, name))
            return name, result
        except RpcFailure as failure:
            assert failure.kind == "revision_conflict"
            assert failure.failure.currentRev == snapshot.rev + 1
            return name, None

    with ThreadPoolExecutor(max_workers=len(clients)) as pool:
        futures = [pool.submit(save, index) for index in range(len(clients))]
        results = [future.result(timeout=10) for future in futures]

    winners = [(name, result) for name, result in results if result is not None]
    assert len(winners) == 1
    name, result = winners[0]
    assert result.rev == snapshot.rev + 1
    assert project_name(clients[0]) == (result.rev, name)


def test_concurrent_resend_of_one_operation_commits_once(daemon):
    """Sending the same save concurrently changes the project only once."""
    clients = [daemon.connect(), daemon.connect()]
    snapshot = clients[0].call("ProjectInspectRequest")
    request = save_request(snapshot, "one-operation")
    barrier = threading.Barrier(2)

    def save(client):
        barrier.wait(timeout=5)
        return client.call("ProjectSaveRequest", timeout=5, **request)

    with ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(save, client) for client in clients]
        results = [future.result(timeout=10) for future in futures]

    assert sorted(result.replayed for result in results) == [False, True]
    assert {result.rev for result in results} == {snapshot.rev + 1}
    assert project_name(clients[0]) == (snapshot.rev + 1, "one-operation")


def test_lost_reply_can_be_recovered_and_replayed_without_another_save(daemon):
    """A lost save reply can be recovered without applying the change twice."""
    observer = daemon.connect()
    snapshot = observer.call("ProjectInspectRequest")
    request = save_request(snapshot, "saved-without-reply")
    with ReplyProxy(daemon.unix, daemon.directory / "proxy") as proxy:
        client = daemon.connect(unix=proxy.unix)
        gate = proxy.drop_reply(50)
        request_id = client.send("ProjectSaveRequest", make("ProjectSaveRequest", **request), request_id=50)
        gate.wait(timeout=5)
        with pytest.raises(TimeoutError):
            client.wait_reply(request_id, timeout=0.05)

        operation = observer.call("OperationGetRequest", opId=request["opId"])
        assert operation.state == OperationState.completed
        assert operation.receipt.resultType == ReceiptValue.SaveResult
        assert operation.receipt.result.rev == snapshot.rev + 1
        replay = observer.call("ProjectSaveRequest", **request)
        assert replay.replayed is True
        assert replay.rev == snapshot.rev + 1
        assert project_name(observer) == (snapshot.rev + 1, "saved-without-reply")


def test_held_reply_does_not_block_other_requests_or_clients(daemon):
    """Holding one save reply leaves other requests and clients usable."""
    observer = daemon.connect()
    snapshot = observer.call("ProjectInspectRequest")
    request = save_request(snapshot, "committed-before-delivery")
    with ReplyProxy(daemon.unix, daemon.directory / "proxy") as proxy:
        client = daemon.connect(unix=proxy.unix)
        gate = proxy.hold_reply(50)
        request_id = client.send("ProjectSaveRequest", make("ProjectSaveRequest", **request), request_id=50)
        gate.wait(timeout=5)

        assert project_name(observer) == (snapshot.rev + 1, "committed-before-delivery")
        # Even this same connection remains usable while one reply is held.
        operation = client.call("OperationGetRequest", timeout=5, opId=request["opId"])
        assert operation.state == OperationState.completed
        assert operation.receipt.resultType == ReceiptValue.SaveResult
        with pytest.raises(TimeoutError):
            client.wait_reply(request_id, timeout=0.05)

        gate.release()
        result = client.wait_reply(request_id, timeout=5)
        assert result.rev == snapshot.rev + 1
        assert result.replayed is False


def test_completed_operation_distinguishes_failure_from_success(client):
    """A completed operation reports whether its save succeeded or failed."""
    snapshot = client.call("ProjectInspectRequest")
    accepted = save_request(snapshot, "accepted")
    client.call("ProjectSaveRequest", **accepted)
    rejected = save_request(snapshot, "must-not-replace-accepted")
    with pytest.raises(RpcFailure) as error:
        client.call("ProjectSaveRequest", **rejected)
    assert error.value.kind == "revision_conflict"

    success = client.call("OperationGetRequest", opId=accepted["opId"])
    failure = client.call("OperationGetRequest", opId=rejected["opId"])
    assert success.state == failure.state == OperationState.completed
    assert success.receipt.resultType == ReceiptValue.SaveResult
    assert failure.receipt.resultType == ReceiptValue.Failure
    assert failure.receipt.result.kind == FailureKind.revision_conflict
    assert project_name(client) == (snapshot.rev + 1, "accepted")


@pytest.mark.parametrize("fault", ["save_after_prepare", "save_during_apply", "save_before_commit"],
                         ids=["after-prepare", "during-apply", "before-commit"])
def test_restart_recovers_the_whole_save_and_its_receipt(core_factory, fault):
    """Restarting after a crash recovers the whole save and its receipt."""
    daemon = core_factory(fault=fault)
    client = daemon.connect()
    snapshot = client.call("ProjectInspectRequest")
    request = save_request(snapshot, "recovered")
    layout = {"cards": {}}
    request["changes"].append(change("SetLayout", json=json.dumps(layout)))
    request_id = client.send("ProjectSaveRequest", make("ProjectSaveRequest", **request))
    with pytest.raises((ConnectionError, TimeoutError)):
        client.wait_reply(request_id, timeout=5)

    daemon.restart()
    recovered = daemon.connect()
    current = recovered.call("ProjectInspectRequest")
    assert current.rev == snapshot.rev + 1
    assert json.loads(text(current.projectJson))["name"] == "recovered"
    assert json.loads(text(current.layoutJson)) == layout
    operation = recovered.call("OperationGetRequest", opId=request["opId"])
    assert operation.state == OperationState.completed
    assert operation.receipt.resultType == ReceiptValue.SaveResult
    result = recovered.call("ProjectSaveRequest", **request)
    assert result.replayed is True
    assert result.rev == current.rev
    assert recovered.call("ProjectInspectRequest").rev == current.rev


def test_notifications_follow_committed_operations_without_replay_duplicates(daemon):
    """Subscribers see each saved change once, even when a save is resent."""
    subscriber = daemon.connect()
    opened = subscriber.open()
    assert opened.seq == 1
    writer = daemon.connect()
    first = save_request(opened.snapshot, "first")
    writer.call("ProjectSaveRequest", **first)

    def patched(op_id):
        return subscriber.wait_notification(
            lambda note: note.valueType == NotificationPayload.DocPatched
            and text(note.value.delta.opId) == op_id,
            timeout=5,
        )

    first_note = patched(first["opId"])
    assert first_note.seq == opened.seq + 1
    assert text(first_note.subscriptionId) == text(opened.subscriptionId)
    assert first_note.value.delta.baseRev == opened.snapshot.rev
    assert first_note.value.delta.rev == opened.snapshot.rev + 1

    replay = writer.call("ProjectSaveRequest", **first)
    assert replay.replayed is True
    second = save_request(writer.call("ProjectInspectRequest"), "second")
    writer.call("ProjectSaveRequest", **second)
    second_note = patched(second["opId"])
    # A duplicate notification from replay would consume this sequence.
    assert second_note.seq == first_note.seq + 1
    assert text(second_note.subscriptionId) == text(opened.subscriptionId)
    assert second_note.value.delta.baseRev == first_note.value.delta.rev
    assert second_note.value.delta.rev == first_note.value.delta.rev + 1
