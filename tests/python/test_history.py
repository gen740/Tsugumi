"""History and artifacts: recordIO x retainArtifact are independent, records
and artifacts survive restarts and CacheClear, reads are paged without loss
or duplication, tampering and path escapes are rejected, and past
invocations never take the state of newer ones."""

from __future__ import annotations

import itertools
import time
from pathlib import Path

import pytest

from conftest import Daemon
from projects import FILE_FBS, card, conn, run_and_wait, write
from tsugumi_client import RpcFailure, new_id, path, text
from Tsugumi.Data.RunState import RunState

BIG = "".join(f"line {i:05d} 日本語テキスト\n" for i in range(20000))  # ~ 600 KB


def project(root: Path, record_io: bool, retain: bool) -> None:
    doc = {"template": "input", "params": [{"name": "value", "type": "string", "data": "definitions/doc/data/big.txt"}],
           "_outputs": FILE_FBS, "recordIO": record_io, "retainArtifact": retain}
    if retain:
        doc["artifactOutput"] = "doc"
    write(root, {"doc": doc, "log": {"template": "logger"}},
          [card("src", "doc"), card("out", "log")], [conn("src", "doc", "out", "in")],
          files={"definitions/doc/data/big.txt": BIG})


def read_all(c, request: str, limit: int, **args) -> bytes:
    """Reads a value/artifact in pages; asserts offsets advance exactly."""
    out, offset = b"", 0
    while True:
        r = c.call(request, offset=offset, limitBytes=limit, **args)
        assert r.offset == offset
        out += bytes(r.bytes)
        assert r.nextOffset == offset + len(r.bytes)
        offset = r.nextOffset
        if r.eof:
            return out


@pytest.mark.parametrize("record_io,retain", list(itertools.product([False, True], repeat=2)))
def test_record_and_artifact_are_independent(tmp_path, record_io, retain):
    root = tmp_path / "p"
    project(root, record_io, retain)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, st = run_and_wait(c, "src")
        assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)

        def check(c):
            rec = c.call("RunRecordRequest", runId=run_id, card=path("src"))
            assert bool(rec.recordingDisabled) == (not record_io)
            assert (len(rec.record.outputs) == 1) == record_io
            arts = c.call("ArtifactListRequest", runId=run_id, card=path("src"), limit=100).artifacts
            assert len(arts) == (1 if retain else 0)
            if retain:
                a = arts[0]
                data = read_all(c, "ArtifactReadRequest", 65536, artifactId=text(a.artifactId))
                assert data.decode() == BIG
            if record_io:
                v = read_all(c, "RunValueReadRequest", 4096, runId=run_id, card=path("src"), invocation=1, side=2, port="doc")
                assert b"big.txt" in v

        check(c)
        # Records and retained artifacts survive CacheClear and a restart.
        c.call("ProjectCacheClearRequest", opId=new_id())
        check(c)
        d.stop()
        d.start()
        c = d.client()
        c.hello()
        check(c)
    finally:
        d.cleanup()


def test_tampered_artifact_and_path_escape_are_rejected(tmp_path):
    root = tmp_path / "p"
    project(root, False, True)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, st = run_and_wait(c, "src")
        assert st.state == RunState.succeeded
        a = c.call("ArtifactListRequest", runId=run_id, limit=10).artifacts[0]
        with pytest.raises(RpcFailure) as e:
            c.call("ArtifactReadRequest", artifactId=text(a.artifactId), member="../../history.sqlite")
        assert e.value.kind in ("invalid_path", "invalid_request", "not_found")
        stored = next((root / ".tsugumi" / "runs" / run_id / "artifacts").rglob("big.txt"))
        stored.write_text("changed")
        with pytest.raises(RpcFailure) as e:
            c.call("ArtifactReadRequest", artifactId=text(a.artifactId))
        assert e.value.kind == "integrity_error"
    finally:
        d.cleanup()


def test_log_paging_keeps_utf8_boundaries(tmp_path):
    root = tmp_path / "p"
    project(root, False, False)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, st = run_and_wait(c, "src")
        assert st.state == RunState.succeeded
        whole, off = "", 0
        while True:
            s = c.call("LogRangeRequest", runId=run_id, stream=3, offset=off, limitBytes=97)
            whole += text(s.text)
            if s.nextOffset == off or s.nextOffset >= s.sizeBytes:
                whole_size = s.sizeBytes
                break
            off = s.nextOffset
        assert len(whole.encode()) == whole_size  # nothing lost, nothing repeated
    finally:
        d.cleanup()


def test_past_invocations_keep_their_own_state(tmp_path):
    """Two runs of the same Card: the older run's record never shows the newer state."""
    root = tmp_path / "p"
    project(root, True, False)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run1, _ = run_and_wait(c, "src")
        time.sleep(0.05)
        run2, _ = run_and_wait(c, "src")
        assert run1 != run2
        r1 = c.call("RunRecordRequest", runId=run1, card=path("src"))
        r2 = c.call("RunRecordRequest", runId=run2, card=path("src"))
        assert text(r1.run.status.runId) == run1 and text(r2.run.status.runId) == run2
        assert r1.record.summary.endedAt <= r2.record.summary.startedAt
    finally:
        d.cleanup()
