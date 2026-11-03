"""Contract tests over the real RPC (spec 6.7)."""

from __future__ import annotations

import json
import struct
import threading
import time

import flatbuffers
import pytest

from conftest import Daemon, write_project
from projects import builtin_project
from tsugumi_client import Client, RpcFailure, change, encode_request, make, new_id, path, text
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.Data.RunState import RunState


def project_json(snapshot) -> dict:
    return json.loads(text(snapshot.projectJson))


def save_name(c: Client, rev: int, name: str, op: str | None = None, base_doc: dict | None = None):
    doc = dict(base_doc or {"pkgsPath": "tsugumi:default", "databaseConnections": [],
                            "canvas": {"cards": [], "connections": []}})
    doc["name"] = name
    return c.call("ProjectSaveRequest", baseRev=rev, opId=op or new_id(), origin="test",
                  changes=[change("SetProject", json=json.dumps(doc))])


def test_basic_contract_example(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        ui = d.client()
        hello = ui.hello()
        project_id = text(hello.projectId)
        opened = ui.open()
        assert opened.seq == 1
        sub = text(opened.subscriptionId)
        rev = opened.snapshot.rev

        agent = d.client()
        agent.hello(project=project_id)
        base = project_json(opened.snapshot)
        op_a = new_id()
        res = save_name(agent, rev, "agent-edit", op_a, base)
        assert res.delta.baseRev == rev and res.delta.rev == rev + 1 and res.replayed is False

        n = ui.wait_notification(lambda n: n.valueType == NotificationPayload.DocPatched)
        assert text(n.subscriptionId) == sub and n.seq == 2
        assert text(n.value.delta.opId) == op_a and n.value.delta.rev == rev + 1

        again = save_name(agent, rev, "agent-edit", op_a, base)
        assert again.replayed is True and again.rev == rev + 1
        time.sleep(0.2)
        assert not any(x.valueType == NotificationPayload.DocPatched for x in ui.notifications)

        with pytest.raises(RpcFailure) as e:
            save_name(ui, rev, "ui-edit", None, base)
        assert e.value.kind == "revision_conflict" and e.value.failure.currentRev == rev + 1

        # Run accepted, connection lost before the reply.
        op_r = new_id()
        run_req = make("RunRequest", baseRev=rev + 1, opId=op_r, origin="ui", card=path("begin"))
        ui.send("RunRequest", run_req)
        ui.close()

        ui2 = d.client()
        ui2.hello()
        ui2.open()
        deadline = time.time() + 10
        while True:
            got = ui2.call("OperationGetRequest", opId=op_r)
            if got.receipt is not None:
                break
            assert time.time() < deadline
            time.sleep(0.05)
        run_id = text(got.receipt.result.run.runId)
        replay = ui2.call("RunRequest", baseRev=rev + 1, opId=op_r, origin="ui", card=path("begin"))
        assert replay.replayed is True and text(replay.run.runId) == run_id
        st = ui2.call("RunStatusRequest", runId=run_id)
        assert text(st.runId) == run_id
        history = ui2.call("RunHistoryRequest")
        assert len(history.runs) == 1
    finally:
        d.cleanup()


def test_operation_conflict_and_required_fields(client):
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


def test_concurrent_saves_one_wins(daemon):
    clients = [daemon.client() for _ in range(6)]
    for c in clients:
        c.hello()
    rev = clients[0].call("ProjectInspectRequest").rev
    results: list = []

    def go(c, i):
        try:
            results.append(("ok", save_name(c, rev, f"n{i}")))
        except RpcFailure as f:
            results.append((f.kind, None))

    threads = [threading.Thread(target=go, args=(c, i)) for i, c in enumerate(clients)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert [r[0] for r in results].count("ok") == 1
    assert all(k in ("ok", "revision_conflict") for k, _ in results)


def test_concurrent_identical_resend_commits_once(daemon):
    a, b = daemon.client(), daemon.client()
    a.hello()
    b.hello()
    op = new_id()
    out = []
    ts = [threading.Thread(target=lambda c=c: out.append(save_name(c, 0, "same", op))) for c in (a, b)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert sorted(r.replayed for r in out) == [False, True]
    assert a.call("ProjectInspectRequest").rev == 1


def test_replay_after_restart(project):
    d = Daemon(project).start()
    op = new_id()
    try:
        c = d.client()
        c.hello()
        save_name(c, 0, "persist", op)
        c.close()
        d.stop()
        d.start()
        c = d.client()
        c.hello()
        again = save_name(c, 0, "persist", op)
        assert again.replayed is True and again.rev == 1
        assert c.call("ProjectInspectRequest").rev == 1
    finally:
        d.cleanup()


@pytest.mark.parametrize("point", ["after_journal", "partial_apply", "before_commit"])
def test_fault_injection_never_leaves_partial_updates(project, point):
    # Two files change in one transaction; a crash at any point must leave both or neither.
    d = Daemon(project, env={"TSUGUMI_ENABLE_FAULTS": "1", "TSUGUMI_FAULT": point}).start()
    doc = json.loads((project / "tsugumi.json").read_text())
    doc["name"] = "crashed"
    try:
        c = d.client()
        c.hello()
        rid = c.send("ProjectSaveRequest", make("ProjectSaveRequest", baseRev=0, opId=new_id(), origin="t", changes=[
            change("SetProject", json=json.dumps(doc)),
            change("SetLayout", json=json.dumps({"cards": {}})),
        ]))
        with pytest.raises((ConnectionError, TimeoutError)):
            c.wait_reply(rid, timeout=10)
        d.proc.wait(timeout=10)
        d.env = {}
        d.start()
        c = d.client()
        c.hello()
        snap = c.call("ProjectInspectRequest")
        on_disk = json.loads((project / "tsugumi.json").read_text())
        layout = json.loads((project / "layout.json").read_text())
        # The journal holds the full transaction, so recovery completes it.
        assert snap.rev == 1
        assert on_disk["name"] == "crashed" and layout == {"cards": {}}
    finally:
        d.cleanup()


def test_document_validation(client):
    snap = client.open().snapshot
    bad = [
        '{"name":"a","name":"b","pkgsPath":"x","databaseConnections":[],"canvas":{"cards":[],"connections":[]}}',
        '{"name":"a","pkgsPath":"x","databaseConnections":[],"canvas":{"cards":[],"connections":[]},"typo":1}',
        '{"name":"a","pkgsPath":"x","databaseConnections":[],"canvas":{"cards":[{"id":"a/b"}],"connections":[]}}',
        '{"name":"a","pkgsPath":"x","databaseConnections":[],"canvas":{"cards":[{"id":"x","definition":"nope"}],"connections":[]}}',
        '{"name":"a","pkgsPath":"x","databaseConnections":[],"canvas":{"cards":[],"connections":[{"from":{"card":"q","port":"o"},"to":{"card":"r","port":"i"}}]}}',
    ]
    for j in bad:
        with pytest.raises(RpcFailure) as e:
            client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
                        changes=[change("SetProject", json=j)])
        assert e.value.kind == "invalid_document", j


def test_uint64_max_and_original_spelling_preserved(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
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
    finally:
        d.cleanup()


def test_explicit_false_is_not_omitted(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        snap = c.call("ProjectInspectRequest")
        files = c.call("ProjectFileListRequest", path=".", atRev=snap.rev, depth=0, includeDirectories=False)
        assert all(text(e.path).count("/") == 0 for e in files.entries)
        files2 = c.call("ProjectFileListRequest", path=".", atRev=snap.rev, depth=3)
        assert any(text(e.path) == "definitions/src/_outputs.fbs" for e in files2.entries)
        plan = c.call("RunPlanRequest", atRev=snap.rev, card=path("src"), propagate=False)
        assert [list(map(text, t.segments)) for t in plan.targets] == [["src"]]
        plan2 = c.call("RunPlanRequest", atRev=snap.rev, card=path("src"))
        assert len(plan2.targets) == 2
    finally:
        d.cleanup()


def test_hello_rules(daemon):
    c = Client(unix=daemon.unix)
    with pytest.raises(RpcFailure) as e:
        c.call("ProjectInspectRequest")
    assert e.value.kind == "unauthenticated"
    c.hello()
    with pytest.raises(RpcFailure) as e:
        c.hello()
    assert e.value.kind == "invalid_request"
    c2 = Client(unix=daemon.unix)
    with pytest.raises(RpcFailure) as e:
        c2.call("SessionHelloRequest", protocol=2, clientId=new_id())
    assert e.value.kind == "protocol_mismatch"
    c3 = Client(unix=daemon.unix)
    with pytest.raises(RpcFailure) as e:
        c3.hello(project=new_id())
    assert e.value.kind == "permission_denied"


def test_unknown_union_closes_connection(daemon):
    c = daemon.client()
    c.hello()
    good = encode_request(5, "ProjectInspectRequest", make("ProjectInspectRequest"))
    # Corrupt the request union tag to an unknown value.
    req = bytearray(good)
    b = flatbuffers.Builder(64)
    from Tsugumi.RPC import Request, Message
    Request.Start(b)
    Request.AddValueType(b, 200)
    r = Request.End(b)
    Message.Start(b)
    Message.AddRequestId(b, 9)
    Message.AddPayloadType(b, 1)
    Message.AddPayload(b, r)
    b.Finish(Message.End(b), file_identifier=b"TSG1")
    c.send_raw(bytes(b.Output()))
    deadline = time.time() + 5
    while not c.closed and time.time() < deadline:
        time.sleep(0.05)
    assert c.closed
    del req
    # Other peers keep working.
    other = daemon.client()
    other.hello()
    other.call("ProjectInspectRequest")


def test_invalid_frame_length_closes(daemon):
    c = daemon.client()
    c._sock.sendall(struct.pack("<I", 17 << 20))
    deadline = time.time() + 5
    while not c.closed and time.time() < deadline:
        time.sleep(0.05)
    assert c.closed


def test_second_daemon_is_refused(daemon):
    second = Daemon(daemon.project).start(expect_exit=True)
    assert second.proc.wait(timeout=10) == 4
    second.cleanup()


def test_source_operations(client):
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
    from Tsugumi.RPC.ViewState import ViewStateT
    from Tsugumi.RPC.Camera import CameraT
    view_id = new_id()
    v = ViewStateT()
    v.scope = path()
    cam = CameraT()
    cam.scope, cam.x, cam.y, cam.zoom = path(), 1.5, -2.0, 1.25
    v.cameras = [cam]
    v.selectedCards = [path("a")]
    r1 = client.call("ViewSaveRequest", viewId=view_id, baseViewRev=0, view=v)
    assert r1.viewRev == 1
    with pytest.raises(RpcFailure) as e:
        client.call("ViewSaveRequest", viewId=view_id, baseViewRev=0, view=v)
    assert e.value.kind == "revision_conflict" and e.value.failure.currentRev == 1
    opened = client.open(view_id=view_id)
    assert opened.snapshot.viewRev == 1
    assert opened.snapshot.view.cameras[0].zoom == 1.25


def test_builtin_run_logs_and_display(tmp_path):
    root = tmp_path / "p"
    builtin_project(root, text="日本語のログ" * 50)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        opened = c.open()
        sub = text(opened.subscriptionId)
        c.call("ViewSetVisibleRequest", subscriptionId=sub, cards=[path("out")])
        acc = c.call("RunRequest", baseRev=opened.snapshot.rev, opId=new_id(), origin="t", card=path("begin"))
        run_id = text(acc.run.runId)
        fin = c.wait_notification(lambda n: n.valueType == NotificationPayload.RunEvent and
                                  n.value.valueType == 5)
        assert fin.value.value.summary.status.state == RunState.succeeded
        disp = c.wait_notification(lambda n: n.valueType == NotificationPayload.DisplayUpdated)
        assert "日本語のログ" in text(disp.value.display.value.text)
        rec = c.call("RunRecordRequest", runId=run_id, card=path("out"))
        assert rec.exists and rec.recordingDisabled is True
        # Read the stdout log in small chunks; never split a character.
        whole = c.call("LogRangeRequest", runId=run_id, card=path("out"), invocation=1, stream=1, limitBytes=262144)
        pieces, off = [], 0
        while True:
            s = c.call("LogRangeRequest", runId=run_id, card=path("out"), invocation=1, stream=1,
                       offset=off, limitBytes=7)
            pieces.append(text(s.text))
            if s.nextOffset == off:
                break
            off = s.nextOffset
            if off >= s.sizeBytes:
                break
        assert "".join(pieces) == text(whole.text)
        tail = c.call("LogRangeRequest", runId=run_id, card=path("out"), invocation=1, stream=1, tail=True, limitBytes=10)
        assert text(whole.text).endswith(text(tail.text)) and tail.truncated
        inside = text(whole.text).encode().index("日".encode()) + 1  # inside a 3-byte character
        with pytest.raises(RpcFailure) as e:
            c.call("LogRangeRequest", runId=run_id, card=path("out"), invocation=1, stream=1, offset=inside)
        assert e.value.kind == "invalid_range"
        with pytest.raises(RpcFailure):
            c.call("LogRangeRequest", runId=run_id, stream=1, tail=True, offset=3)
        journal = c.call("LogRangeRequest", runId=run_id, stream=3)
        lines = [json.loads(x) for x in text(journal.text).splitlines()]
        assert lines[0]["event"] == "started" and lines[-1]["event"] == "succeeded"
    finally:
        d.cleanup()


def test_history_pagination_and_cursor_checks(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        rev = c.call("ProjectInspectRequest").rev
        for _ in range(5):
            c.call("RunRequest", baseRev=rev, opId=new_id(), origin="t", card=path("begin"))
            while True:
                st = c.call("RunStatusRequest")
                if st.state in (RunState.succeeded, RunState.failed, RunState.cancelled):
                    break
                time.sleep(0.02)
        page1 = c.call("RunHistoryRequest", limit=2)
        assert len(page1.runs) == 2 and page1.nextCursor
        # A run added during paging does not appear in the same scan.
        c.call("RunRequest", baseRev=rev, opId=new_id(), origin="t", card=path("begin"))
        time.sleep(0.3)
        seen = [text(r.status.runId) for r in page1.runs]
        cur = text(page1.nextCursor)
        while cur:
            p = c.call("RunHistoryRequest", limit=2, cursor=cur)
            seen += [text(r.status.runId) for r in p.runs]
            cur = text(p.nextCursor) if p.nextCursor else None
        assert len(seen) == 5 and len(set(seen)) == 5
        with pytest.raises(RpcFailure) as e:
            c.call("RunHistoryRequest", limit=2, cursor=text(page1.nextCursor), artifactsOnly=True)
        assert e.value.kind == "invalid_cursor"
    finally:
        d.cleanup()


def test_validate_and_unsupported_format(client):
    snap = client.open().snapshot
    v = client.call("ProjectValidateRequest", atRev=snap.rev)
    assert v.valid is True
    with pytest.raises(RpcFailure) as e:
        client.call("ProjectValidateRequest", atRev=snap.rev + 5)
    assert e.value.kind == "revision_conflict"
    with pytest.raises(RpcFailure) as e:
        client.call("ProjectFileFormatRequest", path="x.fbs", text="table T {}", language=6)
    assert e.value.kind == "unsupported"


def test_format_is_separate_from_saving(client):
    import shutil
    snap = client.open().snapshot
    r = client.call("ProjectFileFormatRequest", path="definitions/a/_card.json", text='{"a":[1,2],"b":{}}')
    assert text(r.text) == '{\n  "a": [\n    1,\n    2\n  ],\n  "b": {}\n}\n'
    bad = client.call("ProjectFileFormatRequest", path="definitions/a/x.json", text='{"a":')
    assert [text(d.code) for d in bad.diagnostics] == ["syntax_error"] and text(bad.text) == '{"a":'
    tools = {"cpp": ("clang-format", "x.cc", "int  main( ){return 0;}", None),
             "python": ("ruff", "x.py", "def f(:\n  pass\n", (1, 7)),
             "nix": ("nixfmt", "x.nix", "{ a = 1; b = [ 1 2 ]; }", None)}
    for lang, (tool, name, src, err_pos) in tools.items():
        if not shutil.which(tool):
            with pytest.raises(RpcFailure) as e:
                client.call("ProjectFileFormatRequest", path="definitions/a/" + name, text=src)
            assert e.value.kind == "unsupported" and tool in str(e.value)
            continue
        res = client.call("ProjectFileFormatRequest", path="definitions/a/" + name, text=src)
        if err_pos:
            assert [text(d.code) for d in res.diagnostics] == ["syntax_error"]
            assert res.diagnostics[0].offset is not None
        else:
            assert not res.diagnostics and text(res.text) != src
    assert client.call("ProjectInspectRequest").rev == snap.rev  # formatting never saves


def test_sdk_prepare_status_and_read(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        rev = c.call("ProjectInspectRequest").rev
        op = new_id()
        acc = c.call("CardSDKPrepareRequest", card=path("src"), atRev=rev, opId=op)
        assert text(acc.opId) == op
        for _ in range(100):
            st = c.call("CardSDKStatusRequest", opId=op)
            if st.state == 2:  # ready
                break
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
        assert got.state == 3 and got.receipt is not None
        # Cache clear never touches documents or history.
        cleared = c.call("ProjectCacheClearRequest", opId=new_id())
        assert "history" in [text(x) for x in cleared.retained]
        assert c.call("ProjectInspectRequest").rev == rev + 1
    finally:
        d.cleanup()


def test_import_through_launcher_grant(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    other = tmp_path / "other"
    builtin_project(other)
    d = Daemon(root).start()
    try:
        grant = new_id()
        (d.control / "grants.json").write_text(json.dumps({"grants": {grant: str(other)}}))
        c = d.client()
        c.hello()
        rev = c.call("ProjectInspectRequest").rev
        res = c.call("ProjectImportRequest", baseRev=rev, opId=new_id(), origin="t", sourceGrant=grant,
                     definitions=["src"])
        mapping = {text(m.from_): text(m.to) for m in res.idMap} if hasattr(res.idMap[0], "from_") else \
                  {text(getattr(m, "from")): text(m.to) for m in res.idMap}
        assert mapping == {"src": "src-2"}
        snap = c.call("ProjectInspectRequest")
        assert any(text(dd.id) == "src-2" for dd in snap.definitions)
        assert any(text(s.path) == "definitions/src-2/_outputs.fbs" for s in snap.sources)
        with pytest.raises(RpcFailure) as e:
            c.call("ProjectImportRequest", baseRev=snap.rev, opId=new_id(), origin="t", sourceGrant=new_id())
        assert e.value.kind == "permission_denied"
    finally:
        d.cleanup()


def test_slow_peer_is_dropped_without_stopping_others(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        import socket as _s
        # A peer that subscribes and then never reads.
        raw = _s.socket(_s.AF_UNIX, _s.SOCK_STREAM)
        raw.setsockopt(_s.SOL_SOCKET, _s.SO_RCVBUF, 4096)
        raw.connect(d.unix)
        for rid, name, obj in [(1, "SessionHelloRequest", make("SessionHelloRequest", protocol=1, clientId=new_id())),
                               (2, "ProjectOpenRequest", make("ProjectOpenRequest"))]:
            frame = encode_request(rid, name, obj)
            raw.sendall(struct.pack("<I", len(frame)) + frame)
        fast = d.client()
        fast.hello()
        fast.open()
        snap = fast.call("ProjectInspectRequest")
        rev, doc = snap.rev, json.loads(text(snap.projectJson))
        big = "x" * 200000
        t0 = time.time()
        for i in range(300):  # ~60 MB of DocPatched for the stalled peer
            doc["name"] = f"{big}{i}"
            rev = fast.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t",
                            changes=[change("SetProject", json=json.dumps(doc))]).rev
        assert rev == 300 and time.time() - t0 < 60
        # The stalled peer was disconnected: draining its socket ends in EOF.
        raw.settimeout(10)
        total = 0
        while True:
            chunk = raw.recv(1 << 20)
            if not chunk:
                break
            total += len(chunk)
        assert total < 300 * 200000
    finally:
        d.cleanup()


def test_ui_layout_fields_and_draft_outputs(client):
    """Groups, note color/size, collapse and hidden output frames are layout
    only (no execution change); a Draft can declare provisional outputs."""
    snap = client.open().snapshot
    layout = {
        "cards": {"src": {"x": 0, "y": 0, "collapsed": True}},
        "notes": [{"id": "n", "scope": "", "text": "**memo**", "color": "blue", "x": 1, "y": 2, "width": 240, "height": 120}],
        "displays": {"src": {"x": 0, "y": 200, "width": 300, "height": 160, "hidden": True}},
        "groups": [{"id": "g", "scope": "", "name": "G", "color": "green", "cards": ["src"],
                    "outputs": [{"card": "src", "port": "msg"}]}],
    }
    res = client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
                      changes=[change("SetLayout", json=json.dumps(layout))])
    assert json.loads(text(res.delta.layoutJson))["groups"][0]["name"] == "G"
    assert res.executionRev == snap.executionRev  # layout never re-resolves computation
    with pytest.raises(RpcFailure) as e:
        client.call("ProjectSaveRequest", baseRev=res.rev, opId=new_id(), origin="t",
                    changes=[change("SetLayout", json=json.dumps({"groups": [{"id": "g", "typo": 1}]}))])
    assert e.value.kind == "invalid_document"

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
    assert [text(p.name) for p in plan.outputs] == ["summary"], [(text(d.code), text(d.message)) for d in plan.diagnostics]
    assert [text(p.name) for p in plan.inputs] == ["in"]
    meta = next(d for d in res2.delta.definitionsUpsert if text(d.id) == "plan")
    assert json.loads(text(meta.metaJson))["context"] == "why"
    v = client.call("ProjectValidateRequest", atRev=res2.rev, targets=[path("plan")])
    assert "draft" in [text(d.code) for d in v.diagnostics]


def test_layout_keeps_flow_note_size_and_boundary_bars(client):
    """layout.json: the direction the Cards were laid out in, a note's text
    size and (in a Card in Card's own layout) its boundary bars."""
    snap = client.call("ProjectInspectRequest")
    layout = {"flow": "leftToRight", "boundary": {"inputs": {"x": 1, "y": 2}},
              "notes": [{"id": "n", "scope": "", "text": "$x$", "x": 0, "y": 0, "fontSize": 20}]}
    res = client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
                      changes=[change("SetLayout", json=json.dumps(layout))])
    assert json.loads(text(res.delta.layoutJson))["notes"][0]["fontSize"] == 20
