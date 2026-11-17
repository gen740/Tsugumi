"""Local file references use the atomic save without an RPC byte payload."""

import hashlib
import json
import re
import sys

import pytest

from tsugumi_client import RpcFailure, change, make, new_id, text


def setup(project):
    card = project / "definitions/data"
    card.mkdir(parents=True)
    (card / "_card.json").write_text('{"template":"draft"}')


def transfer(source, destination="definitions/data/input.csv", **kwargs):
    return make("SourceTransfer", path=destination, from_=source.name,
                sourceRoot=str(source.parent), **kwargs)


def save(client, transfers, **kwargs):
    return client.call("ProjectSaveRequest", baseRev=0, opId=new_id(), origin="agent",
                       changes=[], transfers=transfers, checkPermissions=True, **kwargs)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


@pytest.mark.parametrize("replacement", [False, True])
def test_300_mb_attached_file_has_identical_hash_and_bounded_peak_memory(project, core_factory, tmp_path, replacement):
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
    expected = digest(source)
    if replacement:
        import shutil
        shutil.copyfile(source, project / "definitions/data/input.csv")
        # A different new hash ensures this is an effective replacement.
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
    fields = {"baseHash": digest(project / "definitions/data/input.csv")} if replacement else {}
    result = save(client, [transfer(source, **fields)])
    assert result.rev == 1
    assert text(result.delta.origin) == "agent"
    assert text(result.delta.sourcesUpsert[0].hash) == expected
    assert result.delta.sourcesUpsert[0].sizeBytes == source.stat().st_size
    daemon.stop()

    def peak_memory():
        log = daemon.log.read_text()
        return [int(n) for n in re.findall(r"TSUGUMI_TEST_MAX_RSS_BYTES=(\d+)", log)]

    peaks = peak_memory()
    assert peaks
    # This is the entire process's peak, including startup and shared libraries.
    # A file-sized allocation alone would exceed this bound by more than 2x.
    assert max(peaks) < 128 << 20, peaks
    assert digest(project / "definitions/data/input.csv") == expected
    assert (project / ".tsugumi/document.journal").stat().st_size < 65536
    assert not list((project / ".tsugumi/document-files").iterdir())
    daemon.start()
    assert daemon.connect().call("ProjectInspectRequest").rev == 1
    assert digest(project / "definitions/data/input.csv") == expected
    daemon.stop()
    peaks = peak_memory()
    assert len(peaks) == 2
    assert max(peaks) < 128 << 20, peaks


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

    write(project, {"src": {"template": "input", "params": [
        {"name": "text", "type": "string", "data": "x"},
        {"name": "value", "type": "bool", "data": True}],
        "_outputs": "table Msg { text:string; } table Outputs { msg:Msg (required); } root_type Outputs;"},
        "sink": SINK}, [card_in_card("inner", [card("src", "src"), card("out", "sink")],
            [conn("src", "msg", "out", "in")], {"inputs": {}, "outputs": {}})], [])
    path = "externals/inner/definitions/src/_card.json"
    original = (project / path).read_bytes()
    source = tmp_path / "widened.json"
    body = json.loads(original)
    body["network"] = True
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
    assert json.loads((project / path).read_bytes())["network"] is True


def test_store_manifest_limit_reports_actual_limit_and_attached_alternative(project, core_factory):
    setup(project)
    client = core_factory().connect()
    with pytest.raises(RpcFailure) as error:
        client.call("ProjectSaveRequest", baseRev=0, opId=new_id(), origin="agent",
                    changes=[change("PutSource", path=f"definitions/data/{i}.csv", bytes=[])
                             for i in range(10001)])
    assert error.value.kind == "resource_limit"
    assert "10001" in str(error.value) and "10000" in str(error.value)
    assert "hostFolders" in str(error.value)
    assert client.call("ProjectInspectRequest").rev == 0


def test_store_document_limit_reports_bytes_limit_and_attached_alternative(project, core_factory):
    client = core_factory().connect()
    document = json.loads(text(client.call("ProjectInspectRequest").projectJson))
    document["name"] = "x" * (8 << 20)
    encoded = json.dumps(document)
    with pytest.raises(RpcFailure) as error:
        client.call("ProjectSaveRequest", baseRev=0, opId=new_id(), origin="agent",
                    changes=[change("SetProject", json=encoded)])
    assert error.value.kind == "resource_limit"
    assert str(len(encoded) + 2) in str(error.value)  # layout.json is {}.
    assert "8388608" in str(error.value) and "hostFolders" in str(error.value)


@pytest.mark.parametrize("suffix", ["", "/", "/."])
def test_replaced_attached_root_is_not_followed(project, core_factory, tmp_path, suffix):
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
                sourceRoot=str(granted) + suffix)
    with pytest.raises(RpcFailure) as error:
        save(client, [item])
    assert error.value.kind == "invalid_path"
    assert client.call("ProjectInspectRequest").rev == 0
    assert not (project / "definitions/data/private.csv").exists()


@pytest.mark.parametrize("shell_changes", [False, True])
def test_disconnect_during_streamed_save_discards_all_changes(project, core_factory, tmp_path, shell_changes):
    """Both copy and successful-shell imports use this public cancellable save."""
    import time
    from concurrent.futures import ThreadPoolExecutor
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
        deadline = time.monotonic() + 10
        while not (project / ".tsugumi/transfer.started").exists():
            assert not pending.done(), "save completed before cancellation was observed"
            assert time.monotonic() < deadline
            time.sleep(0.001)
        caller.close()
        with pytest.raises((ConnectionError, OSError)):
            pending.result(timeout=3)
    from Tsugumi.Data.OperationState import OperationState
    deadline = time.monotonic() + 15
    while True:
        outcome = observer.call("OperationGetRequest", opId=operation)
        if outcome.state == OperationState.completed:
            break
        assert time.monotonic() < deadline, "disconnected save never reached a terminal receipt"
        time.sleep(0.01)
    assert observer.call("ProjectInspectRequest").rev == 0
    from Tsugumi.Data.FailureKind import FailureKind
    assert outcome.receipt.result.kind == FailureKind.cancelled
    assert not (project / "definitions/data/input.csv").exists()
    assert not list(staging.iterdir())
