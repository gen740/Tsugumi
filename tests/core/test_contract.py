"""Document, session and history behavior through the real public RPC."""

from contextlib import ExitStack
import hashlib
import json
import shlex
import time

import pytest

from harness.fixtures import worker_project
from harness.proxy import ReplyProxy
from harness.scenario import Project, STRING_OUTPUT, wait_run
from projects import builtin_project, start_run, wait_for_log
from tsugumi_client import Client, RpcFailure, change, make, new_id, path, text
from Tsugumi.Data.OperationState import OperationState
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState
from Tsugumi.Data.SDKState import SDKState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.ReceiptValue import ReceiptValue
from Tsugumi.RPC.RunEventValue import RunEventValue


@pytest.fixture
def builtin_daemon(project, core_factory):
    builtin_project(project)
    return core_factory()


@pytest.fixture
def builtin_client(builtin_daemon):
    return builtin_daemon.connect()


def project_json(snapshot) -> dict:
    return json.loads(text(snapshot.projectJson))


def save_name(c: Client, rev: int, name: str, op: str | None = None, base_doc: dict | None = None):
    doc = dict(base_doc or {"pkgsPath": "tsugumi:default",
                            "canvas": {"cards": [], "connections": []}})
    doc["name"] = name
    return c.call("ProjectSaveRequest", baseRev=rev, opId=op or new_id(), origin="test",
                  changes=[change("SetProject", json=json.dumps(doc))])


def test_lost_run_reply_can_be_recovered_without_starting_another_run(builtin_daemon):
    """A run whose reply was lost can be recovered and retried without running twice."""
    observer = builtin_daemon.connect()
    revision = observer.call("ProjectInspectRequest").rev
    operation = new_id()
    fields = dict(baseRev=revision, opId=operation, origin="ui:test", card=path("start"))
    with ReplyProxy(builtin_daemon.unix, builtin_daemon.directory / "proxy") as proxy:
        starter = builtin_daemon.connect(unix=proxy.unix)
        lost = proxy.drop_reply(50)
        starter.send("RunRequest", make("RunRequest", **fields), request_id=50)
        lost.wait()
        starter.close()

    recovered = observer.call("OperationGetRequest", opId=operation)
    assert recovered.state == OperationState.completed
    assert recovered.receipt.resultType == ReceiptValue.RunAccepted
    run_id = text(recovered.receipt.result.run.runId)
    replay = observer.call("RunRequest", **fields)
    assert replay.replayed is True
    assert text(replay.run.runId) == run_id
    assert wait_run(observer, run_id).state == RunState.succeeded
    history = observer.call("RunHistoryRequest")
    assert [text(entry.status.runId) for entry in history.runs] == [run_id]


def test_completed_save_can_be_replayed_after_restart(daemon):
    """A completed save and its operation receipt survive a normal restart."""
    client = daemon.connect()
    snapshot = client.call("ProjectInspectRequest")
    operation = new_id()
    save_name(client, snapshot.rev, "persisted", operation)
    daemon.restart()
    restarted = daemon.connect()
    replay = save_name(restarted, snapshot.rev, "persisted", operation)
    assert replay.replayed is True and replay.rev == snapshot.rev + 1
    saved = restarted.call("ProjectInspectRequest")
    assert saved.rev == replay.rev and project_json(saved)["name"] == "persisted"


def test_session_requires_hello_and_matching_project(daemon):
    """Hello authenticates a session and checks project identity."""
    with ExitStack() as connections:
        def connect():
            return connections.enter_context(Client(unix=daemon.unix))

        client = connect()
        with pytest.raises(RpcFailure) as error:
            client.call("ProjectInspectRequest")
        assert error.value.kind == "unauthenticated"
        hello = client.hello()
        matching = connect().hello(project=text(hello.projectId))
        assert text(matching.projectId) == text(hello.projectId)
        with pytest.raises(RpcFailure) as error:
            client.hello()
        assert error.value.kind == "invalid_request"
        negotiated = connect().call("SessionHelloRequest")
        capabilities = {text(capability) for capability in negotiated.capabilities}
        assert {f"display:Display{kind}" for kind in
                ("Text", "Scalar", "Table", "Image", "PointCloud", "Module")} <= capabilities
        assert not any(cap.startswith("format:") for cap in capabilities)
        with pytest.raises(RpcFailure) as error:
            connect().hello(project=new_id())
        assert error.value.kind == "permission_denied"


def test_unexpected_save_failure_does_not_leave_a_retry_waiting(core_factory):
    """An unexpected save error is answered again on retry and does not change the project."""
    client = core_factory(fault="save_exception").connect()
    snapshot = client.call("ProjectInspectRequest")
    operation = new_id()
    for _ in range(2):
        with pytest.raises(RpcFailure) as error:
            save_name(client, snapshot.rev, "must-not-save", operation)
        assert error.value.kind == "internal_error"
    assert client.call("ProjectInspectRequest").rev == snapshot.rev


@pytest.mark.parametrize("large_source", [True, False], ids=["large-source", "large-document"])
def test_many_large_saves_keep_their_content_and_latest_receipt_after_restart(daemon, large_source):
    """Many large saves retain the latest document, source and replay result across restart."""
    client = daemon.connect()
    snapshot = client.call("ProjectInspectRequest")
    revision = snapshot.rev
    source_hash = None
    document = project_json(snapshot)
    body = bytes(range(256)) * 4096
    for index in range(12 if large_source else 80):
        if large_source:
            expected = body[index:] + body[:index]
            extra = {"baseHash": source_hash} if source_hash else {}
            changes = [change("PutSource", path="skills/big.bin", bytes=expected, **extra)]
            source_hash = hashlib.sha256(expected).hexdigest()
        else:
            document["name"] = "x" * 200000 + str(index)
            changes = [change("SetProject", json=json.dumps(document))]
        latest = dict(baseRev=revision, opId=new_id(), origin="ui:test", changes=changes)
        revision = client.call("ProjectSaveRequest", **latest).rev

    daemon.restart()
    recovered = daemon.connect()
    saved = recovered.call("ProjectInspectRequest")
    assert saved.rev == revision
    if large_source:
        reply = recovered.call("ProjectFileReadRequest", path="skills/big.bin", atRev=revision,
                               limitBytes=len(expected))
        assert reply.eof and bytes(reply.bytes) == expected
        assert text(reply.hash) == source_hash
    else:
        assert project_json(saved) == document
    replay = recovered.call("ProjectSaveRequest", **latest)
    assert replay.replayed is True and replay.rev == revision
    assert recovered.call("ProjectInspectRequest").rev == revision


def test_operation_conflict_and_required_fields(client):
    """A save rejects reused operation IDs with different content and missing fields."""
    snap = client.open().snapshot
    op = new_id()
    save_name(client, snap.rev, "a", op)
    with pytest.raises(RpcFailure) as e:
        save_name(client, snap.rev, "b", op)
    assert e.value.kind == "operation_conflict"
    with pytest.raises(RpcFailure) as e:
        client.call("ProjectSaveRequest", opId=new_id(), origin="t",
                    changes=[change("SetLayout", json="{}")])
    assert e.value.kind == "invalid_request"
    with pytest.raises(RpcFailure) as e:
        client.call("ProjectSaveRequest", baseRev=1, opId=new_id(), origin="t", changes=[])
    assert e.value.kind == "invalid_request"


def test_document_validation(client):
    """Invalid document fields, card paths and connections cannot be saved."""
    snap = client.open().snapshot
    bad = [
        '{"name":"a","name":"b","pkgsPath":"x","canvas":{"cards":[],"connections":[]}}',
        '{"name":"a","pkgsPath":"x","canvas":{"cards":[],"connections":[]},"typo":1}',
        '{"name":"a","pkgsPath":"x","canvas":{"cards":[{"id":"a/b"}],"connections":[]}}',
        '{"name":"a","pkgsPath":"x","canvas":{"cards":[{"id":"x","definition":"nope"}],"connections":[]}}',
        '{"name":"a","pkgsPath":"x","canvas":{"cards":[],"connections":[{"from":{"card":"q","port":"o"},"to":{"card":"r","port":"i"}}]}}',
    ]
    for j in bad:
        with pytest.raises(RpcFailure) as e:
            client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
                        changes=[change("SetProject", json=j)])
        assert e.value.kind == "invalid_document", j




def test_source_operations(client):
    """Source edits enforce hashes, UTF-8 patch boundaries and permitted paths."""
    snap = client.open().snapshot
    src = "skills/notes.txt"
    body = "こんにちは\n".encode()
    r1 = client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
                     changes=[change("PutSource", path=src, bytes=list(body))])
    h1 = text(r1.delta.sourcesUpsert[0].hash)
    with pytest.raises(RpcFailure) as e:
        client.call("ProjectSaveRequest", baseRev=r1.rev, opId=new_id(), origin="t",
                    changes=[change("PutSource", path=src, bytes=list(b"x"))])
    assert e.value.kind == "hash_conflict" and e.value.args_map["path"] == src
    from Tsugumi.RPC.SourcePatch import SourcePatchT
    bad = SourcePatchT()
    bad.offset, bad.remove, bad.text = 1, 1, "x"  # inside a multi-byte character
    with pytest.raises(RpcFailure) as e:
        client.call("ProjectSaveRequest", baseRev=r1.rev, opId=new_id(), origin="t",
                    changes=[change("PatchSource", path=src, baseHash=h1, patches=[bad])])
    assert e.value.kind == "invalid_range"
    good = SourcePatchT()
    good.offset, good.remove, good.text = 0, 3, "今"
    r2 = client.call("ProjectSaveRequest", baseRev=r1.rev, opId=new_id(), origin="t",
                     changes=[change("PatchSource", path=src, baseHash=h1, patches=[good])])
    content = client.call("ProjectFileReadRequest", path=src, atRev=r2.rev, limitBytes=4)
    assert content.eof is False and content.nextOffset == 4
    rest = client.call("ProjectFileReadRequest", path=src, atRev=r2.rev, offset=4)
    assert (bytes(content.bytes) + bytes(rest.bytes)).decode() == "今んにちは\n"
    for forbidden in ("tsugumi.json", ".tsugumi/x", "../x", "/abs"):
        with pytest.raises(RpcFailure) as e:
            client.call("ProjectSaveRequest", baseRev=r2.rev, opId=new_id(), origin="t",
                        changes=[change("PutSource", path=forbidden, bytes=[1])])
        assert e.value.kind in ("permission_denied", "invalid_path")


def test_external_change_and_reload(daemon, client):
    """Reload imports an external source edit and leaves unchanged projects at the same revision."""
    snap = client.open().snapshot
    r1 = client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
                     changes=[change("PutSource", path="skills/a.txt", bytes=list(b"one"))])
    (daemon.project / "skills" / "a.txt").write_text("edited outside")
    with pytest.raises(RpcFailure) as e:
        client.call("ProjectFileReadRequest", path="skills/a.txt", atRev=r1.rev)
    assert e.value.kind == "external_change"
    rl = client.call("ProjectReloadRequest", baseRev=r1.rev, opId=new_id(), origin="t")
    assert rl.rev == r1.rev + 1
    got = client.call("ProjectFileReadRequest", path="skills/a.txt", atRev=rl.rev)
    assert bytes(got.bytes) == b"edited outside"
    none = client.call("ProjectReloadRequest", baseRev=rl.rev, opId=new_id(), origin="t")
    assert none.delta is None and none.rev == rl.rev


def test_view_save_cas(client):
    """View state persists and rejects edits based on an old view revision."""
    view_id = new_id()
    v = '{ "scope":[], "cameras":[{"zoom":-1}], "client_extension":true }'
    r1 = client.call("ViewSaveRequest", viewId=view_id, baseViewRev=0, viewJson=v)
    assert r1.viewRev == 1
    with pytest.raises(RpcFailure) as e:
        client.call("ViewSaveRequest", viewId=view_id, baseViewRev=0, viewJson=v)
    assert e.value.kind == "revision_conflict" and e.value.failure.currentRev == 1
    opened = client.open(view_id=view_id)
    assert opened.snapshot.viewRev == 1
    assert text(opened.snapshot.viewJson) == v


def test_validate_revision(client):
    """Validation requires the requested revision."""
    snap = client.open().snapshot
    v = client.call("ProjectValidateRequest", atRev=snap.rev)
    assert v.valid is True
    with pytest.raises(RpcFailure) as e:
        client.call("ProjectValidateRequest", atRev=snap.rev + 5)
    assert e.value.kind == "revision_conflict"


def test_ui_layout_fields_and_draft_outputs(client):
    """Groups, note color/size, collapse and hidden output frames are layout
    only (no execution change); a Draft has no ports, even with a schema."""
    snap = client.open().snapshot
    layout = {
        "cards": {"src": {"x": 0, "y": 0, "collapsed": True, "inputOrder": ["b", "a"], "outputOrder": ["out"]}},
        "notes": [{"id": "n", "scope": "", "text": "**memo**", "color": "blue", "x": 1, "y": 2, "width": 240, "height": 120}],
        "displays": {"src": {"x": 0, "y": 200, "width": 300, "height": 160, "hidden": True}},
        "groups": [{"id": "g", "scope": "", "name": "G", "color": "green", "cards": ["src"],
                    "outputs": [{"card": "src", "port": "msg"}]}],
    }
    res = client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
                      changes=[change("SetLayout", json=json.dumps(layout))])
    assert json.loads(text(res.delta.layoutJson))["groups"][0]["name"] == "G"
    assert res.executionRev == snap.executionRev  # layout never re-resolves computation
    assert json.loads(text(res.delta.layoutJson))["cards"]["src"]["inputOrder"] == ["b", "a"]

    doc = project_json(client.call("ProjectInspectRequest"))
    doc["canvas"]["cards"].append({"id": "plan"})
    fbs = 'namespace d;\ntable SummaryValue { value:string; }\ntable Outputs { summary:SummaryValue; }\nroot_type Outputs;\n'
    res2 = client.call("ProjectSaveRequest", baseRev=res.rev, opId=new_id(), origin="t", changes=[
        change("PutDefinition", id="plan", cardJson=json.dumps({"template": "draft", "inputs": [{"name": "in"}]}),
               metaJson=json.dumps({"description": "d", "content": "p", "context": "why"})),
        change("PutSource", path="definitions/plan/_outputs.fbs", bytes=list(fbs.encode())),
        change("SetProject", json=json.dumps(doc)),
    ])
    plan = next(r for r in res2.delta.resolution if [text(s) for s in r.card.segments] == ["plan"])
    assert [text(p.name) for p in plan.outputs] == [], [(text(d.code), text(d.message)) for d in plan.diagnostics]
    assert [text(p.name) for p in plan.inputs] == []
    meta = next(d for d in res2.delta.definitionsUpsert if text(d.id) == "plan")
    assert json.loads(text(meta.metaJson))["context"] == "why"
    v = client.call("ProjectValidateRequest", atRev=res2.rev, targets=[path("plan")])
    assert "draft" in [text(d.code) for d in v.diagnostics]


def test_uint64_max_and_original_spelling_preserved(builtin_client):
    """Unsigned 64-bit values retain their exact spelling and overflow is diagnosed."""
    c = builtin_client
    snap = c.open().snapshot
    doc = project_json(snap)
    doc["canvas"]["cards"].append({"id": "b"})
    raw = '{"template": "input", "params": [{"name": "n", "type": "uint64", "data": 18446744073709551615}]}'
    res = c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
                 changes=[change("SetProject", json=json.dumps(doc)), change("PutDefinition", id="b", cardJson=raw)])
    assert "18446744073709551615" in text(next(x for x in res.delta.definitionsUpsert if text(x.id) == "b").cardJson)
    diags = [text(x.code) for r in res.delta.resolution for x in r.diagnostics
             if text(r.card.segments[0]) == "b"]
    assert "invalid_param" not in diags
    # One more than uint64 max is a type error diagnostic, not a silent truncation.
    raw2 = raw.replace("18446744073709551615", "18446744073709551616")
    res2 = c.call("ProjectSaveRequest", baseRev=res.rev, opId=new_id(), origin="t",
                  changes=[change("PutDefinition", id="b", cardJson=raw2)])
    diags2 = [text(x.code) for r in res2.delta.resolution for x in r.diagnostics
              if text(r.card.segments[0]) == "b"]
    assert "invalid_param" in diags2


def test_explicit_false_is_not_omitted(builtin_client):
    """Explicit zero depth and disabled propagation differ from their request defaults."""
    c = builtin_client
    snap = c.call("ProjectInspectRequest")
    files = c.call("ProjectFileListRequest", path=".", atRev=snap.rev, depth=0, includeDirectories=False)
    assert all(text(e.path).count("/") == 0 for e in files.entries)
    files2 = c.call("ProjectFileListRequest", path=".", atRev=snap.rev, depth=3)
    assert any(text(e.path) == "definitions/src/_outputs.fbs" for e in files2.entries)
    plan = c.call("RunPlanRequest", atRev=snap.rev, card=path("src"), propagate=False)
    assert [list(map(text, t.segments)) for t in plan.targets] == [["src"]]
    plan2 = c.call("RunPlanRequest", atRev=snap.rev, card=path("src"))
    assert len(plan2.targets) == 2


def test_history_pagination_and_cursor_checks(builtin_client):
    """History pages keep a stable scan while new runs finish and reject malformed cursors."""
    c = builtin_client
    rev = c.call("ProjectInspectRequest").rev
    for _ in range(5):
        accepted = c.call("RunRequest", baseRev=rev, opId=new_id(), origin="t", card=path("start"))
        assert wait_run(c, text(accepted.run.runId)).state == RunState.succeeded
    page1 = c.call("RunHistoryRequest", limit=2)
    assert len(page1.runs) == 2 and page1.nextCursor
    # A run added during paging does not appear in the same scan.
    accepted = c.call("RunRequest", baseRev=rev, opId=new_id(), origin="t", card=path("start"))
    assert wait_run(c, text(accepted.run.runId)).state == RunState.succeeded
    seen = [text(r.status.runId) for r in page1.runs]
    cur = text(page1.nextCursor)
    while cur:
        p = c.call("RunHistoryRequest", limit=2, cursor=cur)
        assert len(seen) < 5, "history pagination must terminate after the original five runs"
        seen += [text(r.status.runId) for r in p.runs]
        cur = text(p.nextCursor) if p.nextCursor else None
    assert len(seen) == 5 and len(set(seen)) == 5
    with pytest.raises(RpcFailure) as e:
        c.call("RunHistoryRequest", limit=2, cursor="not-a-cursor")
    assert e.value.kind == "invalid_cursor"


def test_sdk_prepare_status_and_read(builtin_client):
    """Prepared SDK files can be read and become stale after the project changes."""
    c = builtin_client
    rev = c.call("ProjectInspectRequest").rev
    op = new_id()
    acc = c.call("CardSDKPrepareRequest", card=path("src"), atRev=rev, opId=op)
    assert text(acc.opId) == op
    # SDK preparation has a status RPC but emits no completion notification.
    deadline = time.monotonic() + 5
    while True:
        st = c.call("CardSDKStatusRequest", opId=op)
        if st.state == SDKState.ready:
            break
        assert time.monotonic() < deadline, "SDK preparation did not finish"
        time.sleep(0.02)
    names = {text(f.path).rsplit("/", 1)[1]: f for f in st.files}
    assert {"_inputs.fbs", "_outputs.fbs", "_params.fbs"} <= set(names)
    got = c.call("ProjectFileReadRequest", path=text(names["_outputs.fbs"].path), atRev=rev)
    assert b"table Outputs" in bytes(got.bytes)
    # Replay returns the receipt; an unknown job is not_found.
    again = c.call("CardSDKPrepareRequest", card=path("src"), atRev=rev, opId=op)
    assert text(again.opId) == op
    with pytest.raises(RpcFailure) as e:
        c.call("CardSDKStatusRequest", opId=new_id())
    assert e.value.kind == "not_found"
    # A later revision makes the prepared SDK stale for FileRead.
    doc = json.loads(text(c.call("ProjectInspectRequest").projectJson))
    doc["name"] = "changed"
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t", changes=[change("SetProject", json=json.dumps(doc))])
    with pytest.raises(RpcFailure) as e:
        c.call("ProjectFileReadRequest", path=text(names["_outputs.fbs"].path), atRev=rev + 1)
    assert e.value.kind == "revision_conflict"
    # Operation receipts are visible through OperationGet.
    got = c.call("OperationGetRequest", opId=op)
    assert got.state == OperationState.completed and got.receipt is not None
    # Cache clear never touches documents or history.
    cleared = c.call("ProjectCacheClearRequest", opId=new_id())
    assert "history" in [text(x) for x in cleared.retained]
    assert c.call("ProjectInspectRequest").rev == rev + 1


def test_live_log_truncation_reaches_record_and_empty_streams(project, tmp_path, core_factory):
    from Tsugumi.Data.CardState import CardState

    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    import os
    release = tmp_path / "release"
    os.mkfifo(release)
    body = f"""
        head -c {136 << 20} /dev/zero | tr '\\000' x
        read -r release < {shlex.quote(str(release))}
        printf '%s' '{{"out":{{"value":"ok"}}}}' > "$TSUGUMI_OUTPUT"
    """
    Project(project).card("work", {"language": "bash", "sandbox": False}, outputs=STRING_OUTPUT,
                          manifest={"run": {"bash": body}}).save(client)
    run_id = start_run(client, "work", propagate=False)
    args = dict(runId=run_id, card=path("work"), invocation=1)
    limit = 128 << 20
    try:
        stderr = wait_for_log(client, run_id, "work", LogStream.standard_error,
                              lambda log: log.logTruncatedAtBytes is not None)
        assert stderr.logTruncatedAtBytes == limit and not stderr.truncated
        live = client.call("RunRecordRequest", **args).record.summary
        assert live.state == CardState.running
        assert live.logTruncatedAtBytes == limit
        stdout = client.call("LogRangeRequest", **args, stream=LogStream.standard_output, tail=True)
        assert 120 << 20 < stdout.sizeBytes <= limit
    finally:
        if client.call("RunStatusRequest", runId=run_id).state == RunState.running:
            with release.open("w") as gate:
                gate.write("release\n")
    assert wait_run(client, run_id).state == RunState.succeeded
    journal = client.call("LogRangeRequest", **args, stream=LogStream.journal)
    assert journal.logTruncatedAtBytes == limit
    for line in text(journal.text).splitlines():
        assert isinstance(json.loads(line), dict)


def test_run_logs_and_display(project, tmp_path, core_factory):
    """A worker run publishes its display, paged UTF-8 log and completed history."""
    message = "日本語のログ" * 50
    c = core_factory(mocked_builds=True, mocked_worker=True).connect()
    worker_project(c, project, tmp_path, [
        {"op": "display_text", "text": message},
        {"op": "log", "stream": "stdout", "text": message},
    ], recordIO=False)
    shown = path("work")
    opened = c.open()
    sub = text(opened.subscriptionId)
    c.call("ViewSetVisibleRequest", subscriptionId=sub, cards=[shown])
    acc = c.call("RunRequest", baseRev=opened.snapshot.rev, opId=new_id(), origin="t", card=shown)
    run_id = text(acc.run.runId)
    fin = c.wait_notification(lambda n: n.valueType == NotificationPayload.RunEvent and
                              n.value.valueType == RunEventValue.RunFinished)
    assert fin.value.value.summary.status.state == RunState.succeeded
    disp = c.wait_notification(lambda n: n.valueType == NotificationPayload.DisplayUpdated)
    assert "日本語のログ" in text(disp.value.display.value.text)
    rec = c.call("RunRecordRequest", runId=run_id, card=shown)
    assert rec.exists and rec.recordingDisabled is True
    # Read the stdout log in small chunks; never split a character.
    whole = c.call("LogRangeRequest", runId=run_id, card=shown, invocation=1, stream=LogStream.standard_output, limitBytes=262144)
    assert text(whole.text) == message + "\n"
    pieces, off = [], 0
    while True:
        s = c.call("LogRangeRequest", runId=run_id, card=shown, invocation=1, stream=LogStream.standard_output,
                   offset=off, limitBytes=7)
        pieces.append(text(s.text))
        if s.nextOffset == off:
            break
        off = s.nextOffset
        if off >= s.sizeBytes:
            break
    assert "".join(pieces) == text(whole.text)
    tail = c.call("LogRangeRequest", runId=run_id, card=shown, invocation=1, stream=LogStream.standard_output, tail=True, limitBytes=10)
    assert text(whole.text).endswith(text(tail.text)) and tail.truncated
    inside = text(whole.text).encode().index("日".encode()) + 1  # inside a 3-byte character
    with pytest.raises(RpcFailure) as e:
        c.call("LogRangeRequest", runId=run_id, card=shown, invocation=1, stream=LogStream.standard_output, offset=inside)
    assert e.value.kind == "invalid_range"
    with pytest.raises(RpcFailure):
        c.call("LogRangeRequest", runId=run_id, stream=LogStream.standard_output, tail=True, offset=3)
    journal = c.call("LogRangeRequest", runId=run_id, stream=LogStream.journal)
    assert text(journal.text)
    history = c.call("RunHistoryRequest")
    assert text(history.runs[0].status.runId) == run_id
    assert history.runs[0].status.state == RunState.succeeded



def test_an_empty_optional_document_is_not_given(client):
    """A model fills in every field of a change. An empty link_json is no link,
    and an empty meta_json keeps the file, which may hold the user's intent."""
    snap = client.call("ProjectInspectRequest")
    doc = project_json(snap)
    doc["canvas"]["cards"].append({"id": "plan"})
    meta = json.dumps({"description": "d", "content": "the user's intent"})
    card = {"template": "draft", "inputs": [{"name": "in"}]}
    res = client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t", changes=[
        change("PutDefinition", id="plan", cardJson=json.dumps(card), metaJson=meta),
        change("SetProject", json=json.dumps(doc)),
    ])
    card["inputs"].append({"name": "more"})
    blank = change("PutDefinition", id="plan", cardJson=json.dumps(card), metaJson="", linkJson="")
    # The same in a check and in a save.
    v = client.call("ProjectValidateRequest", changes=[blank])
    assert "invalid_document" not in [text(d.code) for d in v.diagnostics]
    res = client.call("ProjectSaveRequest", baseRev=res.rev, opId=new_id(), origin="t", changes=[blank])
    saved = next(d for d in res.delta.definitionsUpsert if text(d.id) == "plan")
    assert json.loads(text(saved.cardJson))["inputs"][1]["name"] == "more"
    assert saved.linkJson is None
    assert json.loads(text(saved.metaJson))["content"] == "the user's intent"
    # Leaving it out still removes it, as the contract says.
    res = client.call("ProjectSaveRequest", baseRev=res.rev, opId=new_id(), origin="t", changes=[
        change("PutDefinition", id="plan", cardJson=json.dumps(card))])
    assert next(d for d in res.delta.definitionsUpsert if text(d.id) == "plan").metaJson is None




def test_draft_resolution_ignores_meta_schema_and_layout(client):
    """A Draft remains portless across metadata, schema and layout edits."""
    def ports(res, card):
        r = next(r for r in res.delta.resolution if [text(s) for s in r.card.segments] == [card])
        return [(text(p.name), text(p.rootType) if p.rootType else "") for p in r.outputs]
    snap = client.call("ProjectInspectRequest")
    doc = project_json(snap)
    doc["canvas"]["cards"].append({"id": "plan"})
    draft = json.dumps({"template": "draft", "inputs": [{"name": "in"}]})
    res = client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t", changes=[
        change("PutDefinition", id="plan", cardJson=draft, metaJson=json.dumps({"description": "d", "outputs": [{"name": "first"}]})),
        change("SetProject", json=json.dumps(doc))])
    assert ports(res, "plan") == []
    res = client.call("ProjectSaveRequest", baseRev=res.rev, opId=new_id(), origin="t",
                      changes=[change("SetLayout", json=json.dumps({"cards": {"plan": {"x": 5, "y": 5}}}))])
    assert ports(res, "plan") == []
    res = client.call("ProjectSaveRequest", baseRev=res.rev, opId=new_id(), origin="t", changes=[
        change("PutDefinition", id="plan", cardJson=draft, metaJson=json.dumps({"description": "d", "outputs": [{"name": "second"}]}))])
    assert ports(res, "plan") == []
    fbs = "namespace d;\ntable {t} {{ value:string; }}\ntable Outputs {{ summary:{t}; }}\nroot_type Outputs;\n"
    prev = None
    for t in ("One", "Two"):
        body = fbs.format(t=t).encode()
        extra = {"baseHash": hashlib.sha256(prev).hexdigest()} if prev else {}
        res = client.call("ProjectSaveRequest", baseRev=res.rev, opId=new_id(), origin="t",
                          changes=[change("PutSource", path="definitions/plan/_outputs.fbs", bytes=list(body), **extra)])
        assert ports(res, "plan") == []
        prev = body
