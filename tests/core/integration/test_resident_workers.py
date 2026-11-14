"""Resident state belongs to an explicitly reusing Card, not to a bounded cache."""

from harness.real_cards import bash, log
from projects import card, run_and_wait, write
from tsugumi_client import new_id, path, text
from Tsugumi.Data.RunState import RunState


def test_more_than_32_residents_keep_their_state(core_factory, tmp_path):
    """A 33rd resident does not evict the first Card's worker or its state."""
    definition = bash('f="${TSUGUMI_WORK_DIR%/*}/home/count"; n=0; '
                      'test ! -f "$f" || read -r n < "$f"; n=$((n+1)); echo "$n" > "$f"; '
                      'printf \'{"text":"%s"}\' "$n" > "$TSUGUMI_REPORT"', reuseWorker=True)
    cards = [card("c" + str(i), "resident") for i in range(33)]
    write(tmp_path / "p", {"resident": definition}, cards, [])
    c = core_factory(project_root=tmp_path / "p").connect()
    for name in ["c" + str(i) for i in range(33)] + ["c0"]:
        run_id, st = run_and_wait(c, name)
        assert st.state == RunState.succeeded, log(c, run_id, name, 2)
    inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("c0")).records
    assert inv.workerReused
    record = c.call("RunRecordRequest", runId=run_id, card=path("c0")).record
    assert text(record.display.value.text) == "2"


def test_resident_definition_change_records_fresh_state(core_factory, tmp_path):
    """The next invocation records a definition replacement and worker_reused is false."""
    root = tmp_path / "p"
    definition = bash('echo first', reuseWorker=True)
    write(root, {"resident": definition}, [card("resident", "resident")], [])
    c = core_factory(project_root=root).connect()
    first, st = run_and_wait(c, "resident")
    assert st.state == RunState.succeeded
    assert "state started fresh" not in log(c, first, "resident", 2)
    write(root, {"resident": bash('echo changed', reuseWorker=True)}, [card("resident", "resident")], [])
    c.call("ProjectReloadRequest", baseRev=c.call("ProjectInspectRequest").rev, opId=new_id(), origin="test")
    second, st = run_and_wait(c, "resident")
    assert st.state == RunState.succeeded
    inv, = c.call("RunInvocationsRequest", runId=second, card=path("resident")).records
    assert not inv.workerReused
    assert "state started fresh: the Card definition changed" in log(c, second, "resident", 2)


def test_dead_resident_is_reported_in_the_next_invocation(core_factory, tmp_path):
    """A resident killed by Card code is identified as fresh state on the next attempt."""
    definition = bash('kill -KILL "$PPID"', reuseWorker=True)
    write(tmp_path / "p", {"resident": definition}, [card("resident", "resident")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    first, st = run_and_wait(c, "resident")
    assert st.state == RunState.failed
    second, st = run_and_wait(c, "resident")
    assert st.state == RunState.failed
    inv, = c.call("RunInvocationsRequest", runId=second, card=path("resident")).records
    assert not inv.workerReused
    assert "state started fresh: the resident worker died" in log(c, second, "resident", 2)


def test_living_resident_is_discarded_after_a_card_failure(core_factory, tmp_path):
    """Even a cooperative failure discards state and records why the next worker is fresh."""
    definition = bash('f="${TSUGUMI_WORK_DIR%/*}/home/failed"; '
                      'if test ! -e "$f"; then touch "$f"; exit 19; fi; echo recovered', reuseWorker=True)
    write(tmp_path / "p", {"resident": definition}, [card("resident", "resident")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    first, st = run_and_wait(c, "resident")
    assert st.state == RunState.failed
    second, st = run_and_wait(c, "resident")
    assert st.state == RunState.failed
    inv, = c.call("RunInvocationsRequest", runId=second, card=path("resident")).records
    assert not inv.workerReused
    assert "state started fresh: the previous call failed" in log(c, second, "resident", 2)


def test_output_transfer_failure_keeps_living_resident_state(core_factory, tmp_path):
    """Rejecting a partial special file does not discard the resident's independent state."""
    from projects import FILE_FBS
    from Tsugumi.Data.FailureKind import FailureKind
    definition = bash('f="${TSUGUMI_WORK_DIR%/*}/home/called"; '
                      'if test ! -e "$f"; then touch "$f"; mkfifo "$TSUGUMI_OUTPUT_DIR/result"; '
                      'else echo recovered > "$TSUGUMI_OUTPUT_DIR/result"; fi; '
                      'printf \'{"doc":{"value":"%s/result"}}\' "$TSUGUMI_OUTPUT_DIR" > "$TSUGUMI_OUTPUT"',
                      outputs=FILE_FBS, reuseWorker=True)
    write(tmp_path / "p", {"resident": definition}, [card("resident", "resident")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    first, st = run_and_wait(c, "resident")
    assert st.state == RunState.failed
    inv, = c.call("RunInvocationsRequest", runId=first, card=path("resident")).records
    assert inv.failure.kind == FailureKind.output_invalid
    second, st = run_and_wait(c, "resident")
    assert st.state == RunState.succeeded, log(c, second, "resident", 2)
    inv, = c.call("RunInvocationsRequest", runId=second, card=path("resident")).records
    assert inv.workerReused
