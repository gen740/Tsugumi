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
