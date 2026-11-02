"""Reproductions of reported core defects; each must stay fixed.

1. A run whose invocation records could not be saved must not succeed.
2. File values pass through fork/branch (forwarding an input is allowed).
3. Saved inputs for a single-Card run keep File contents across CacheClear and restarts.
4. A run stalled by a full downstream queue ends instead of hanging
   (needs a code Card feedback loop: test_cards_container.py).
"""

from __future__ import annotations

import json
import time
from pathlib import Path

from conftest import Daemon
from projects import card, conn
from tsugumi_client import RpcFailure, new_id, path, text
from Tsugumi.Data.RunState import RunState

FILE_FBS = """
namespace files;
table Doc { value:string; }
table Outputs { doc:Doc (required, tsugumi: "file"); }
root_type Outputs;
attribute "tsugumi";
"""


def put(p: Path, text: str) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text)


def write(root: Path, defs: dict, cards: list, conns: list, files: dict | None = None) -> None:
    root.mkdir(parents=True, exist_ok=True)
    for d, body in defs.items():
        (root / "definitions" / d).mkdir(parents=True, exist_ok=True)
        card_json = {k: v for k, v in body.items() if k != "_outputs"}
        put(root / "definitions" / d / "_card.json", json.dumps(card_json))
        if "_outputs" in body:
            put(root / "definitions" / d / "_outputs.fbs", body["_outputs"])
    for rel, data in (files or {}).items():
        put(root / rel, data)
    put(root / "tsugumi.json", json.dumps({
        "name": "regression", "pkgsPath": "tsugumi:default", "databaseConnections": [],
        "canvas": {"cards": cards, "connections": conns}}))
    put(root / "layout.json", "{}")


def run_and_wait(c, card_id: str, propagate: bool = True, timeout: float = 30.0):
    snap = c.call("ProjectInspectRequest")
    acc = c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path(card_id), propagate=propagate)
    run_id = text(acc.run.runId)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        st = c.call("RunStatusRequest", runId=run_id)
        if st.state in (RunState.succeeded, RunState.failed, RunState.cancelled):
            return run_id, st
        time.sleep(0.05)
    raise TimeoutError(f"run {run_id} did not finish within {timeout}s")


def file_fork_project(root: Path) -> None:
    write(root,
          defs={"trigger": {"template": "trigger"},
                "doc": {"template": "input", "params": [{"name": "value", "type": "string", "data": "definitions/doc/data/report.txt"}],
                        "_outputs": FILE_FBS},
                "fork": {"template": "fork"},
                "log": {"template": "logger"}},
          cards=[card("src", "doc"), card("split", "fork"), card("a", "log"), card("b", "log")],
          conns=[conn("src", "doc", "split", "in"), conn("split", "out1", "a", "in"), conn("split", "out2", "b", "in")],
          files={"definitions/doc/data/report.txt": "quarterly numbers\n"})


def test_history_write_failure_fails_the_run(tmp_path):
    root = tmp_path / "p"
    file_fork_project(root)
    d = Daemon(root, env={"TSUGUMI_ENABLE_FAULTS": "1", "TSUGUMI_FAULT": "history_invocation"}).start()
    try:
        c = d.client()
        c.hello()
        _, st = run_and_wait(c, "src")
        assert st.state == RunState.failed
        assert st.failure.kind == 25  # persistence_failed
        for k in ("src", "split", "a", "b"):
            try:
                page = c.call("RunInvocationsRequest", runId=text(st.runId), card=path(k))
            except RpcFailure as e:
                assert e.kind == "not_found"  # nothing recorded for it
                continue
            assert all(r.state != 6 for r in page.records)  # nothing unrecorded claims success
    finally:
        d.cleanup()


def test_file_passes_through_fork(tmp_path):
    root = tmp_path / "p"
    file_fork_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, st = run_and_wait(c, "src")
        assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
        for k in ("a", "b"):
            log = c.call("LogRangeRequest", runId=run_id, card=path(k), invocation=1, stream=1)
            assert "report.txt" in text(log.text)
    finally:
        d.cleanup()


def test_saved_file_inputs_survive_cache_clear_and_restart(tmp_path):
    root = tmp_path / "p"
    file_fork_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        _, st = run_and_wait(c, "src")
        assert st.state == RunState.succeeded
        # The produced file lives in the run's work area, which CacheClear removes.
        c.call("ProjectCacheClearRequest", opId=new_id())
        _, st2 = run_and_wait(c, "split", propagate=False)
        assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
        d.stop()
        d.start()
        c = d.client()
        c.hello()
        _, st3 = run_and_wait(c, "split", propagate=False)
        assert st3.state == RunState.succeeded, st3.failure and text(st3.failure.diagnostic)
        # The logger downstream also kept its own saved input.
        _, st4 = run_and_wait(c, "a", propagate=False)
        assert st4.state == RunState.succeeded
    finally:
        d.cleanup()


def test_saved_inputs_missing_port_fails(tmp_path):
    root = tmp_path / "p"
    file_fork_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        _, st = run_and_wait(c, "split", propagate=False)
        assert st.state == RunState.failed
        assert "saved" in text(st.failure.diagnostic)
    finally:
        d.cleanup()


def test_rerun_from_saved_file_inputs_with_much_shorter_paths(tmp_path):
    """Saved copies of a File input (per invocation and per port) can have a
    shorter path than the original; rewriting the path must shrink the string
    without corrupting the buffer (flatbuffers' SetString cannot shrink)."""
    # The producer's work path contains its Card id; a long id makes the saved copies much shorter.
    src = "source-" + "x" * 80
    root = tmp_path / "p"
    write(root,
          defs={"doc": {"template": "input", "params": [{"name": "value", "type": "string", "data": "definitions/doc/report.txt"}],
                        "_outputs": FILE_FBS},
                "join": {"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}]},
                "log": {"template": "logger"}},
          cards=[card(src, "doc"), card("join", "join"), card("out", "log")],
          conns=[conn(src, "doc", "join", "a"), conn("join", "out", "out", "in")],
          files={"definitions/doc/report.txt": "quarterly numbers\n"})
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        _, st = run_and_wait(c, src)
        assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
        # The merge reruns from its saved value; the logger shows the path it received.
        run_id, st2 = run_and_wait(c, "join")
        assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
        log = c.call("LogRangeRequest", runId=run_id, card=path("out"), invocation=1, stream=1)
        received = json.loads(text(log.text))["value"]["value"]
        assert received.endswith("/a/report.txt") and Path(received).read_text() == "quarterly numbers\n"
    finally:
        d.cleanup()
