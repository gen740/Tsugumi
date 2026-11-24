"""Local file references use the atomic save without an RPC byte payload."""

import hashlib
import json

import re
import sys

import pytest

from tsugumi_client import RpcFailure, change, make, new_id, text
from harness.control import read_startup_error
from harness.document import build_buffer, card_document, card_readme, layout_bytes


def setup(project):
    card = project / "definitions/data"
    card.mkdir(parents=True)
    (card / "_card.json").write_text(json.dumps(card_document({"type": "draft"})))
    (card / "README.md").write_text(card_readme("Data"))
    config = project / "tsugumi.json"
    document = json.loads(config.read_text())
    document["canvas"]["cards"] = [{"id": "data"}]
    config.write_text(json.dumps(document))


def transfer(source, destination="definitions/data/input.csv", **kwargs):
    return make("SourceTransfer", path=destination, from_=source.name,
                sourceRoot=str(source.parent), **kwargs)


def save(client, transfers, **kwargs):
    return client.call("ProjectSaveRequest", baseRev=0, opId=new_id(), origin="agent",
                       changes=[], transfers=transfers, checkPermissions=True, **kwargs)


def test_oversized_save_reply_leaves_transferred_document_unwritten(project, core_factory, tmp_path):
    from projects import card_in_card, write

    write(project, {}, [card_in_card("inner", [], [], {})], [])
    client = core_factory().connect()
    before = client.call("ProjectInspectRequest")
    source = tmp_path / "layout.bin"
    source.write_bytes(layout_bytes({}) + bytes(7 << 20))
    path = "externals/inner/layout.bin"
    item = transfer(source, path)
    op_id = new_id()
    # The transferred text isn't part of the incoming frame; the reply has
    # both that text and the caller's undo data.
    with pytest.raises(RpcFailure) as error:
        client.call("ProjectSaveRequest", baseRev=before.rev, opId=op_id,
                    origin="test", changes=[], transfers=[item],
                    operationData=bytes(10 << 20))
    assert error.value.kind == "resource_limit"
    assert not (project / path).exists()
    assert client.call("ProjectInspectRequest").rev == before.rev
    receipt = client.call("OperationGetRequest", opId=op_id).receipt
    assert receipt is not None
    succeeded = save(client, [item])
    assert succeeded.rev == before.rev + 1
    assert (project / path).read_bytes() == source.read_bytes()


def test_accumulating_saves_leave_the_snapshot_openable(project, core_factory):
    from projects import card, card_in_card, write

    names = ("first", "second", "last")
    write(project, {}, [card("data"), *(card_in_card(name, [], [], {}) for name in names)], [])
    # Long paths consume frame space without counting towards the JSON budget.
    folder = project / "definitions/data"
    for _ in range(7):
        folder /= "d" * 100
    folder.mkdir(parents=True)
    for i in range(9400):
        (folder / (f"{i:04}" + "f" * 96)).touch()
    daemon = core_factory()
    client = daemon.connect()
    before = client.call("ProjectInspectRequest")
    view_id = new_id()
    view = build_buffer("tsugumi", "Tsugumi.RPC.ClientView", {})
    view += bytes((1 << 20) - len(view))
    client.call("ViewSaveRequest", viewId=view_id, baseViewRev=0, view=view)
    rev = before.rev
    for name in names[:2]:
        saved = client.call(
            "ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="test",
            changes=[change("PutSource", path=f"externals/{name}/layout.bin",
                            bytes=layout_bytes({}) + bytes((3 << 20) - 32))])
        rev = saved.rev
    path = "externals/last/layout.bin"
    with pytest.raises(RpcFailure) as error:
        client.call(
            "ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="test",
            changes=[change("PutSource", path=path,
                            bytes=layout_bytes({}) + bytes((7 << 18) - 32))])
    assert error.value.kind == "resource_limit"
    assert not (project / path).exists()
    daemon.restart()
    reopened = daemon.connect()
    assert reopened.call("ProjectInspectRequest").rev == rev
    snapshot = reopened.call("ProjectOpenRequest", viewId=view_id).snapshot
    assert snapshot.rev == rev and bytes(snapshot.view) == view


def test_reload_stops_at_the_document_budget_and_identifies_the_path(project, core_factory):
    daemon = core_factory()
    client = daemon.connect()
    before = client.call("ProjectInspectRequest")
    paths = {f"externals/{name}/layout.bin" for name in ("a", "b", "c")}
    for relative in paths:
        path = project / relative
        path.parent.mkdir(parents=True)
        path.write_bytes(layout_bytes({}) + bytes((3 << 20) - 32))
    with pytest.raises(RpcFailure) as error:
        client.call("ProjectReloadRequest", baseRev=before.rev,
                    opId=new_id(), origin="test")
    assert error.value.kind == "resource_limit"
    assert error.value.args_map["path"] in paths
    assert client.call("ProjectInspectRequest").rev == before.rev
    daemon.stop()
    daemon.start(expect_exit=True)
    assert daemon.proc.returncode != 0
    startup = read_startup_error(daemon.control / "server.startup-error")
    assert startup["kind"] == "resource_limit"
    assert any(path in startup["message"] for path in paths)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def test_300_mb_attached_file_has_identical_hash_and_bounded_peak_memory(project, core_factory, tmp_path):
    """A 300 MB file crosses no frame and neither save nor reopen buffers it."""
    setup(project)
    attached = tmp_path / "attached"
    attached.mkdir()
    source = attached / "huge.csv"
    chunk = b"123,456\n" * 8192
    remaining = 300_000_000
    with source.open("wb") as output:
        while remaining:
            part = chunk[:remaining]
            output.write(part)
            remaining -= len(part)
    target = project / "definitions/data/input.csv"
    target.write_bytes(b"previous content")
    with source.open("r+b") as output:
        output.write(b"NEW")
    expected = digest(source)
    daemon = core_factory()
    daemon.stop()
    wrapper = tmp_path / "measure-core"
    wrapper.write_text(f"""#!{sys.executable}
import os, signal, subprocess, sys
child = subprocess.Popen([{str(daemon.binary)!r}, *sys.argv[1:]])
for sig in (signal.SIGTERM, signal.SIGINT):
    signal.signal(sig, lambda sig, frame: child.send_signal(sig))
_, status, usage = os.wait4(child.pid, 0)
child.returncode = os.waitstatus_to_exitcode(status)
peak = usage.ru_maxrss * (1 if sys.platform == 'darwin' else 1024)
print(f'TSUGUMI_TEST_MAX_RSS_BYTES={{peak}}', flush=True)
sys.exit(child.returncode)
""")
    wrapper.chmod(0o700)
    daemon.binary = wrapper
    daemon.start()
    client = daemon.connect()
    result = save(client, [transfer(source, baseHash=digest(target))])
    assert text(result.delta.sourcesUpsert[0].hash) == expected
    daemon.stop()

    def peak_memory():
        log = daemon.log.read_text()
        return [int(n) for n in re.findall(r"TSUGUMI_TEST_MAX_RSS_BYTES=(\d+)", log)]

    save_peaks = peak_memory()
    daemon.start()
    assert daemon.connect().call("ProjectInspectRequest").rev == result.rev
    assert digest(target) == expected
    daemon.stop()
    reopen_peaks = peak_memory()[len(save_peaks):]
    # A single file-sized allocation would exceed this bound on either path.
    assert save_peaks and reopen_peaks
    assert max(save_peaks + reopen_peaks) < 128 << 20


def test_later_transfer_failure_applies_nothing_and_cleans_staging(project, core_factory, tmp_path):
    setup(project)
    source = tmp_path / "first.csv"
    source.write_bytes(b"first")
    daemon = core_factory()
    client = daemon.connect()
    with pytest.raises(RpcFailure) as error:
        save(client, [transfer(source), transfer(tmp_path / "missing.csv", "definitions/data/other.csv")])
    assert error.value.kind == "invalid_path"
    assert client.call("ProjectInspectRequest").rev == 0
    assert not (project / "definitions/data/input.csv").exists()
    assert not list((project / ".tsugumi/document-files").iterdir())


@pytest.mark.parametrize("fault", ["save_after_prepare", "save_during_apply", "save_before_commit"])
def test_streamed_save_recovers_all_files_and_replays_receipt(project, core_factory, tmp_path, fault):
    setup(project)
    source = tmp_path / "first.csv"
    source.write_bytes(b"recover me")
    daemon = core_factory(fault=fault)
    client = daemon.connect()
    operation = new_id()
    request = dict(baseRev=0, opId=operation, origin="agent", changes=[],
                   transfers=[transfer(source), transfer(source, "definitions/data/second.csv")])
    with pytest.raises((ConnectionError, TimeoutError)):
        client.call("ProjectSaveRequest", **request)
    # The input may disappear after the durable intent: recovery owns its copy.
    source.unlink()
    daemon.restart()
    recovered = daemon.connect()
    assert recovered.call("ProjectInspectRequest").rev == 1
    assert (project / "definitions/data/input.csv").read_bytes() == b"recover me"
    assert (project / "definitions/data/second.csv").read_bytes() == b"recover me"
    assert recovered.call("ProjectSaveRequest", **request).replayed
    assert not (project / ".tsugumi/document-files").exists()


def test_confirmed_source_conflict_leaves_no_destination(project, core_factory, tmp_path):
    setup(project)
    source = tmp_path / "changed.csv"
    source.write_bytes(b"changed")
    client = core_factory().connect()
    with pytest.raises(RpcFailure) as error:
        save(client, [transfer(source, expectedHash=hashlib.sha256(b"original").hexdigest())])
    assert error.value.kind == "external_change"
    assert not (project / "definitions/data/input.csv").exists()
    assert not list((project / ".tsugumi/document-files").iterdir())


def test_host_selected_source_root_and_more_than_1000_files(project, core_factory, tmp_path):
    setup(project)
    mounted = tmp_path / "mounted"
    mounted.mkdir()
    source = mounted / "data.csv"
    source.write_bytes(b"mounted data")
    client = core_factory().connect()
    transfers = [make("SourceTransfer", path=f"definitions/data/{i}.csv", from_="data.csv",
                      sourceRoot=str(mounted))
                 for i in range(1001)]
    result = save(client, transfers)
    assert result.rev == 1
    assert len(result.delta.sourcesUpsert) == 1001
    assert (project / "definitions/data/1000.csv").read_bytes() == b"mounted data"


def test_transfer_permission_widening_still_requires_explicit_approval(project, core_factory, tmp_path):
    from projects import SINK, card, card_in_card, conn, write

    write(project, {"src": {"type": "code", "params": {"text": "x", "value": True}, "_params": '{ text = ""; value = false; }',
        "_outputs": "table Msg { text:string; } table Outputs { msg:Msg (required); } root_type Outputs;"},
        "sink": SINK}, [card_in_card("inner", [card("src", "src"), card("out", "sink")],
            [conn("src", "msg", "out", "in")], {"inputs": {}, "outputs": {}})], [])
    path = "externals/inner/definitions/src/_card.json"
    original = (project / path).read_bytes()
    source = tmp_path / "widened.json"
    body = json.loads(original)
    body["grants"]["network"] = True
    source.write_text(json.dumps(body))
    client = core_factory().connect()
    item = transfer(source, path, baseHash=hashlib.sha256(original).hexdigest())
    with pytest.raises(RpcFailure) as error:
        save(client, [item])
    assert error.value.kind == "approval_required"
    assert (project / path).read_bytes() == original
    assert not list((project / ".tsugumi/document-files").iterdir())
    result = save(client, [item], approvedPermissions=["network inner/src"])
    assert result.rev == 1
    assert json.loads((project / path).read_bytes())["grants"]["network"] is True


def test_store_manifest_limit_rejects_the_save(project, core_factory):
    setup(project)
    client = core_factory().connect()
    original = client.call("ProjectInspectRequest")
    with pytest.raises(RpcFailure) as error:
        client.call("ProjectSaveRequest", baseRev=original.rev, opId=new_id(), origin="agent",
                    changes=[change("PutSource", path=f"definitions/data/{i}.csv", bytes=[])
                             for i in range(10001)])
    assert error.value.kind == "resource_limit"
    snapshot = client.call("ProjectInspectRequest")
    assert snapshot.rev == original.rev
    assert {text(source.path): text(source.hash) for source in snapshot.sources} == {
        text(source.path): text(source.hash) for source in original.sources
    }


def test_store_document_limit_rejects_the_save(project, core_factory):
    client = core_factory().connect()
    original = client.call("ProjectInspectRequest")
    document = json.loads(text(original.projectJson))
    encoded = json.dumps({**document, "name": "x" * (8 << 20)})
    with pytest.raises(RpcFailure) as error:
        client.call("ProjectSaveRequest", baseRev=original.rev, opId=new_id(), origin="agent",
                    changes=[change("SetProject", json=encoded)])
    assert error.value.kind == "resource_limit"
    snapshot = client.call("ProjectInspectRequest")
    assert snapshot.rev == original.rev
    assert json.loads(text(snapshot.projectJson)) == document


def test_replaced_attached_root_is_not_followed(project, core_factory, tmp_path):
    setup(project)
    granted = tmp_path / "granted"
    granted.mkdir()
    outside = tmp_path / "other-project"
    outside.mkdir()
    (outside / "private.csv").write_text("private")
    client = core_factory().connect()
    granted.rmdir()
    granted.symlink_to(outside, target_is_directory=True)
    item = make("SourceTransfer", path="definitions/data/private.csv", from_="private.csv",
                sourceRoot=str(granted) + "/.")
    with pytest.raises(RpcFailure) as error:
        save(client, [item])
    assert error.value.kind == "invalid_path"
    assert client.call("ProjectInspectRequest").rev == 0
    assert not (project / "definitions/data/private.csv").exists()


@pytest.mark.parametrize("shell_changes", [False, True])
def test_disconnect_during_streamed_save_discards_all_changes(project, core_factory, tmp_path, shell_changes):
    """Both copy and successful-shell imports use this public cancellable save."""
    from concurrent.futures import ThreadPoolExecutor
    from harness.file_events import wait_for_file
    setup(project)
    source = (project / ".tsugumi/agent-files/test/commands/success/data.bin"
              if shell_changes else tmp_path / "attached/data.bin")
    source.parent.mkdir(parents=True)
    with source.open("wb") as output:
        output.truncate(2 << 16)
    daemon = core_factory(fault="transfer_gate")
    caller, observer = daemon.connect(), daemon.connect()
    operation = new_id()
    staging = project / ".tsugumi/document-files"
    with ThreadPoolExecutor(max_workers=1) as threads:
        pending = threads.submit(caller.call, "ProjectSaveRequest", baseRev=0, opId=operation,
                                 origin="agent", changes=[], transfers=[transfer(source)])
        wait_for_file(project / ".tsugumi/transfer.started")
        assert not pending.done(), "save completed before cancellation was observed"
        caller.close()
        with pytest.raises((ConnectionError, OSError)):
            pending.result()
    from Tsugumi.Data.OperationState import OperationState
    # ProjectOpen runs on the mutation lane behind the cancelled save.
    observer.open()
    outcome = observer.call("OperationGetRequest", opId=operation)
    assert outcome.state == OperationState.completed
    assert observer.call("ProjectInspectRequest").rev == 0
    from Tsugumi.Data.FailureKind import FailureKind
    assert outcome.receipt.result.kind == FailureKind.cancelled
    assert not (project / "definitions/data/input.csv").exists()
    assert not list(staging.iterdir())
