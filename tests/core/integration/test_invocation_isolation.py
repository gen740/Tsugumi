"""Invocation data boundaries on native Linux/VM bubblewrap and macOS Seatbelt."""

import shlex

from harness.real_cards import bash, log
from projects import FILE_FBS, card, conn, run_and_wait, write
from tsugumi_client import new_id, path, text
from Tsugumi.Data.RunState import RunState


def test_only_wired_files_are_visible_and_worker_reuse_survives(core_factory, tmp_path):
    """A reused worker reads changing wired inputs but cannot open another Card's recorded File by path."""
    root = tmp_path / "p"
    make = bash('echo wired > "$TSUGUMI_OUTPUT_DIR/data"; '
                'printf \'{"doc":{"value":"%s/data"}}\' "$TSUGUMI_OUTPUT_DIR" > "$TSUGUMI_OUTPUT"',
                outputs=FILE_FBS, recordIO=True)
    # First produce an independently recorded File with no wire to the reader.
    write(root, {"make": make}, [card("secret", "make")], [])
    c = core_factory(project_root=root).connect()
    secret_run, st = run_and_wait(c, "secret", timeout=600)
    assert st.state == RunState.succeeded, log(c, secret_run, "secret", 2)
    record = c.call("RunRecordRequest", runId=secret_run, card=path("secret"))
    secret = text(record.record.outputs[0].filePath)
    assert secret, "the attack needs the independently recorded File's real path"
    reader = bash('test ! -e /proc/self/fd/900 || exit 4; if IFS= read -r stolen < ' + shlex.quote(secret) + '; then echo leaked; exit 1; fi; '
                  'IFS= read -r line < "$FILE_DOC"; test "$line" = wired || exit 2; '
                  'if echo corrupt >> "$FILE_DOC"; then exit 3; fi; '
                  'echo "input=$FILE_DOC"; '
                  'printf \'{"doc":{"value":"%s"}}\' "$FILE_DOC" > "$TSUGUMI_OUTPUT"',
                  inputs=["doc"], outputs=FILE_FBS, reuseWorker=True, recordIO=True)
    write(root, {"make": make, "read": reader},
          [card("secret", "make"), card("source", "make"), card("reader", "read")],
          [conn("source", "doc", "reader", "doc")])
    c.call("ProjectReloadRequest", baseRev=c.call("ProjectInspectRequest").rev, opId=new_id(), origin="test")
    seen = []
    for _ in range(2):
        run_id, st = run_and_wait(c, "source", timeout=600)
        assert st.state == RunState.succeeded, log(c, run_id, "reader", 2)
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("reader")).records
        seen.append(inv.workerReused)
        assert "leaked" not in log(c, run_id, "reader")
        # Input forwarding is mapped back before the staging area is emptied.
        forwarded = c.call("RunRecordRequest", runId=run_id, card=path("reader"))
        assert not forwarded.record.outputs[0].fileMissing
    assert seen == [False, True]


def test_python_source_loading_has_the_invocation_environment_and_log(core_factory, tmp_path):
    """The real Python shim imports under preRun's environment and logs to the invocation."""
    from harness.real_cards import python_probe_project, shown_by_probe
    source = '''
import os
assert os.environ["FROM_PRE"] == "ready"
print("source run=" + os.environ["TSUGUMI_RUN_ID"], flush=True)
def main(inputs, report):
    report.text("loaded")
'''
    definitions, cards, wires, files = python_probe_project(source, "export FROM_PRE=ready")
    write(tmp_path / "p", definitions, cards, wires, files)
    c = core_factory(project_root=tmp_path / "p").connect()
    first, shown = shown_by_probe(c)
    assert shown == "loaded"
    assert "source run=" + first in log(c, first, "show")
    second, shown = shown_by_probe(c)
    assert shown == "loaded" and "source run=" not in log(c, second, "show")


def test_python_source_loading_is_killed_at_the_invocation_deadline(core_factory, tmp_path):
    """An uncooperative import is timed out with its source log attached to the failed invocation."""
    from harness.real_cards import python_probe_project
    from Tsugumi.Data.FailureKind import FailureKind
    source = '''
import os, time
print("source run=" + os.environ["TSUGUMI_RUN_ID"], flush=True)
time.sleep(60)
def main(inputs, report):
    raise AssertionError("main must not run")
'''
    definitions, cards, wires, files = python_probe_project(source)
    definitions["show"]["timeoutSeconds"] = 5
    write(tmp_path / "p", definitions, cards, wires, files)
    c = core_factory(project_root=tmp_path / "p").connect()
    run_id, st = run_and_wait(c, "start", timeout=1800)
    assert st.state == RunState.failed
    invocation, = c.call("RunInvocationsRequest", runId=run_id, card=path("show")).records
    assert invocation.failure.kind == FailureKind.timeout
    assert "source run=" + run_id in log(c, run_id, "show")


def test_reused_worker_clears_previous_input_and_output_slots(core_factory, tmp_path):
    """Only the first call has first.txt and first-output; neither survives in the reused worker's fixed slots."""
    make = bash('n="${TSUGUMI_WORK_DIR%/*}/home/count"; i=0; test ! -f "$n" || read -r i < "$n"; '
                'i=$((i+1)); echo "$i" > "$n"; '
                'if test "$i" = 1; then name=first.txt; else name=second.txt; fi; '
                'echo "$name" > "$TSUGUMI_OUTPUT_DIR/$name"; '
                'printf \'{"doc":{"value":"%s/%s"}}\' "$TSUGUMI_OUTPUT_DIR" "$name" > "$TSUGUMI_OUTPUT"',
                outputs=FILE_FBS, reuseWorker=True)
    reader = bash('slot="${FILE_DOC%/*}"; '
                  'if test -f "$FILE_DOC" && test "${FILE_DOC##*/}" = first.txt; then '
                  'echo first > "$TSUGUMI_OUTPUT_DIR/first-output"; '
                  'else test "${FILE_DOC##*/}" = second.txt || exit 11; '
                  'test ! -e "$slot/first.txt" || exit 12; '
                  'test ! -e "$TSUGUMI_OUTPUT_DIR/first-output" || exit 13; fi; '
                  'echo current > "$TSUGUMI_OUTPUT_DIR/current"; '
                  'printf \'{"doc":{"value":"%s/current"}}\' "$TSUGUMI_OUTPUT_DIR" > "$TSUGUMI_OUTPUT"',
                  inputs=["doc"], outputs=FILE_FBS, reuseWorker=True)
    root = tmp_path / "p"
    write(root, {"make": make, "read": reader}, [card("make", "make"), card("read", "read")],
          [conn("make", "doc", "read", "doc")])
    c = core_factory(project_root=root).connect()
    for reused in (False, True):
        run_id, st = run_and_wait(c, "make")
        assert st.state == RunState.succeeded, log(c, run_id, "read", 2)
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("read")).records
        assert inv.workerReused == reused


def test_failed_call_with_fifo_preserves_its_failure(core_factory, tmp_path):
    """A partial FIFO output cannot replace the Card's exit failure with a transfer error."""
    from Tsugumi.Data.FailureKind import FailureKind
    failed = bash('mkfifo "$TSUGUMI_OUTPUT_DIR/partial"; exit 19', outputs=FILE_FBS)
    write(tmp_path / "p", {"fail": failed}, [card("fail", "fail")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    run_id, st = run_and_wait(c, "fail")
    assert st.state == RunState.failed
    inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("fail")).records
    assert inv.failure.kind == FailureKind.worker_failed
    assert "exit status 19" in text(inv.failure.diagnostic)


def test_cancelled_call_with_fifo_preserves_cancellation(core_factory, tmp_path):
    """Cancellation skips partial output transfer, even when the worker left a FIFO."""
    from projects import start_run, wait_run
    from Tsugumi.Data.FailureKind import FailureKind
    import time
    waiting = bash('mkfifo "$TSUGUMI_OUTPUT_DIR/partial"; echo ready; sleep 60', outputs=FILE_FBS)
    write(tmp_path / "p", {"wait": waiting}, [card("wait", "wait")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    run_id = start_run(c, "wait")
    until = time.monotonic() + 600
    while "ready" not in log(c, run_id, "wait"):
        assert time.monotonic() < until, "worker did not reach cancellation point"
        time.sleep(0.02)
    c.call("RunCancelRequest", runId=run_id)
    assert wait_run(c, run_id, timeout=30).state == RunState.cancelled
    inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("wait")).records
    assert inv.failure.kind == FailureKind.cancelled


def test_worker_staging_uses_disk_scratch_and_releases_call_bytes(core_factory, tmp_path):
    """Input/output staging is below disk run scratch; only lifetime home survives a call."""
    from pathlib import Path
    # The output path reveals the configured disk scratch without depending on
    # whether the daemon is running on Linux or macOS.
    make = bash('echo wired > "$TSUGUMI_OUTPUT_DIR/data"; '
                'printf \'{"doc":{"value":"%s/data"}}\' "$TSUGUMI_OUTPUT_DIR" > "$TSUGUMI_OUTPUT"',
                outputs=FILE_FBS)
    read = bash('printf \'{"text":"%s|%s"}\' "$FILE_DOC" "$TSUGUMI_OUTPUT_DIR" > "$TSUGUMI_REPORT"; '
                'cat "$FILE_DOC" > "$TSUGUMI_OUTPUT_DIR/copied"; '
                'printf \'{"doc":{"value":"%s/copied"}}\' "$TSUGUMI_OUTPUT_DIR" > "$TSUGUMI_OUTPUT"',
                inputs=["doc"], outputs=FILE_FBS, reuseWorker=True)
    root = tmp_path / "p"
    write(root, {"make": make, "read": read}, [card("make", "make"), card("read", "read")],
          [conn("make", "doc", "read", "doc")])
    c = core_factory(project_root=root).connect()
    run_id, st = run_and_wait(c, "make")
    assert st.state == RunState.succeeded, log(c, run_id, "read", 2)
    record = c.call("RunRecordRequest", runId=run_id, card=path("read")).record
    recorded = Path(text(record.outputs[0].filePath))
    # <scratch>/runs/<run>/<card>/<invocation>/out/copied
    scratch = next(parent.parent for parent in recorded.parents if parent.name == "runs")
    staged_input, staged_out = text(record.display.value.text).split("|")
    worker_root = Path(staged_out).parent
    assert worker_root.parent == scratch / "workers"
    assert Path(staged_input).is_relative_to(worker_root / "inputs")
    assert recorded.read_text() == "wired\n"
    assert not list((worker_root / "inputs").iterdir())
    assert not list((worker_root / "out").iterdir())


def test_python_source_environment_removals_survive_reuse(core_factory, tmp_path):
    """Removing an SDK default during import stays removed in Python and libc on both calls."""
    from harness.real_cards import python_probe_project, shown_by_probe
    source = '''
import ctypes, os
assert os.environ.pop("MPLBACKEND", None) is not None
libc = ctypes.CDLL(None)
libc.getenv.argtypes = [ctypes.c_char_p]
libc.getenv.restype = ctypes.c_char_p
def main(inputs, report):
    assert "MPLBACKEND" not in os.environ
    assert libc.getenv(b"MPLBACKEND") is None
    report.text("removed")
'''
    definitions, cards, wires, files = python_probe_project(source)
    write(tmp_path / "p", definitions, cards, wires, files)
    c = core_factory(project_root=tmp_path / "p").connect()
    for reused in (False, True):
        run_id, shown = shown_by_probe(c)
        assert shown == "removed"
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("show")).records
        assert inv.workerReused == reused
