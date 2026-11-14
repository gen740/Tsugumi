"""Resident state belongs to an explicitly reusing Card, not to a bounded cache."""

import pytest

from harness.real_cards import bash, log
from harness.scenario import Project
from projects import card, run_and_wait, write
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState


def test_more_than_32_residents_keep_their_state(core_factory, tmp_path):
    """A 33rd resident does not evict the first Card's worker or its state."""
    definition = bash('f="${TSUGUMI_WORK_DIR%/*}/home/count"; n=0; '
                      'test ! -f "$f" || read -r n < "$f"; n=$((n+1)); echo "$n" > "$f"; '
                      'printf \'{"out":{"value":%s}}\' "$n" > "$TSUGUMI_OUTPUT"; '
                      'printf \'{"text":"%s"}\' "$n" > "$TSUGUMI_REPORT"', reuseWorker=True)
    cards = [card("c" + str(i), "resident") for i in range(33)]
    write(tmp_path / "p", {"resident": definition}, cards, [])
    c = core_factory(project_root=tmp_path / "p").connect()
    seen = set()
    for name in ["c" + str(i) for i in range(33)] + ["c0"]:
        run_id, st = run_and_wait(c, name)
        assert st.state == RunState.succeeded, log(c, run_id, name, 2)
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path(name)).records
        assert inv.workerReused == (name in seen)
        record = c.call("RunRecordRequest", runId=run_id, card=path(name)).record
        assert text(record.display.value.text) == ("2" if name in seen else "1")
        seen.add(name)


def test_resident_definition_change_records_fresh_state(core_factory, tmp_path):
    """The next invocation records a definition replacement and worker_reused is false."""
    root = tmp_path / "p"
    write(root, {}, [], [])
    c = core_factory(project_root=root).connect()
    graph = Project(root)
    for greeting, count, reused in [("first", "1", False), ("first", "2", True), ("changed", "1", False)]:
        definition = bash('echo ' + greeting + '; '
                          'f="${TSUGUMI_WORK_DIR%/*}/home/count"; n=0; '
                          'test ! -f "$f" || read -r n < "$f"; n=$((n+1)); echo "$n" > "$f"; '
                          'printf \'{"out":{"value":%s}}\' "$n" > "$TSUGUMI_OUTPUT"; '
                          'printf \'{"text":"%s"}\' "$n" > "$TSUGUMI_REPORT"', reuseWorker=True)
        # Save both the definition and its Nix source through the public RPC.
        graph.card("resident", {"language": "bash", "reuseWorker": True},
                   outputs=definition["_outputs"], sources={"default.nix": definition["_nix"]})
        graph.save(c)
        run_id, st = run_and_wait(c, "resident")
        assert st.state == RunState.succeeded, log(c, run_id, "resident", 2)
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("resident")).records
        assert inv.workerReused == reused
        record = c.call("RunRecordRequest", runId=run_id, card=path("resident")).record
        assert text(record.display.value.text) == count
        assert greeting in log(c, run_id, "resident")
        if greeting == "changed":
            assert "state started fresh: the Card definition changed" in log(c, run_id, "resident", 2)
        else:
            assert "state started fresh" not in log(c, run_id, "resident", 2)


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


def test_missing_required_output_discards_resident_state(core_factory, tmp_path):
    """A result rejected for a missing required output cannot keep its mutated resident state."""
    from Tsugumi.Data.FailureKind import FailureKind
    definition = bash('f="${TSUGUMI_WORK_DIR%/*}/home/called"; '
                      'if test ! -e "$f"; then touch "$f"; echo \'{}\' > "$TSUGUMI_OUTPUT"; '
                      'else printf \'{"out":{"value":2}}\' > "$TSUGUMI_OUTPUT"; fi',
                      reuseWorker=True, recordIO=True)
    write(tmp_path / "p", {"resident": definition}, [card("resident", "resident")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    for _ in range(2):
        run_id, st = run_and_wait(c, "resident")
        assert st.state == RunState.failed, log(c, run_id, "resident", 2)
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("resident")).records
        assert inv.failure.kind == FailureKind.output_invalid
        assert "required output out is missing" in text(inv.failure.diagnostic)
        assert not inv.workerReused


@pytest.mark.parametrize("stop", ["cancel", "deadline", "shutdown"])
def test_stopped_resident_can_interrupt_a_large_send(core_factory, tmp_path, stop):
    """A child stops an idle resident; its next oversized frame still permits cancellation, timeout and shutdown."""
    import signal
    import time
    from pathlib import Path
    from harness.real_cards import NUM
    from projects import conn, start_run, wait_run
    from Tsugumi.Data.CardState import CardState
    from Tsugumi.Data.FailureKind import FailureKind

    source = r'''
#include <tsugumi/sdk/abi.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
int tsugumi_main(const tsugumi_call* c) {
  char gate[4096], stopped[4096], sending[4096];
  snprintf(gate, sizeof gate, "%s/../home/stop", c->working_directory);
  snprintf(stopped, sizeof stopped, "%s/../home/stopped", c->working_directory);
  snprintf(sending, sizeof sending, "%s/../home/sending", c->working_directory);
  const pid_t worker = getpid();
  pid_t child = fork();
  if (child < 0) return 1;
  if (!child) {
    while (access(gate, F_OK) != 0) usleep(10000);
    if (kill(worker, SIGSTOP) != 0) _exit(2);
    FILE* ready = fopen(stopped, "w");
    if (ready) fclose(ready);
    // fd 3 is the inherited WorkerCall socket. Do not drain it: the stopped
    // worker must leave the oversized frame's sender blocked.
    struct pollfd data = {3, POLLIN, 0};
    if (poll(&data, 1, -1) != 1 || !(data.revents & POLLIN)) _exit(3);
    ready = fopen(sending, "w");
    if (ready) fclose(ready);
    _exit(0);
  }
  c->display_text(c->host, gate);
  return c->emit_json(c->host, "out", "{\"value\":1}");
}
'''
    definition = {
        "language": "cpp", "inputs": [{"name": "data"}], "reuseWorker": True,
        "recordIO": True, "_outputs": NUM,
        "_nix": '''{ pkgs, inputs, params }: {
  run = pkgs.runCommandCC "stopped-resident" {} ''
    mkdir -p $out/lib
    $CC -shared -fPIC -I${inputs.tsugumi.runtime}/include ${./src/main.c} -o $out/lib/main.so
  '';
}
''',
    }
    if stop == "deadline":
        definition["timeoutSeconds"] = 5
    # The second frame must exceed a Unix socket's send buffer, on both hosts.
    payload = {"template": "input", "params": [{"name": "value", "type": "string", "data": "x" * (4 << 20)}],
               "_outputs": 'table Text { value:string; } table Outputs { data:Text (required); } root_type Outputs;'}
    root = tmp_path / "p"
    write(root, {"payload": payload, "resident": definition},
          [card("source", "payload"), card("resident", "resident")], [conn("source", "data", "resident", "data")],
          {"definitions/resident/src/main.c": source})
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    first, st = run_and_wait(c, "source")
    assert st.state == RunState.succeeded, log(c, first, "resident", 2)
    record = c.call("RunRecordRequest", runId=first, card=path("resident")).record
    gate = Path(text(record.display.value.text))
    gate.touch()
    until = time.monotonic() + 10
    while not gate.with_name("stopped").exists():
        assert time.monotonic() < until, "child did not stop its resident worker"
        time.sleep(0.01)
    second = start_run(c, "source")
    until = time.monotonic() + 60
    # `running` is already visible during preparation, before acquiring a
    # worker. Wait for actual frame bytes without letting the worker read them.
    while not gate.with_name("sending").exists():
        assert time.monotonic() < until, "resident did not begin receiving its second frame"
        time.sleep(0.01)
    if stop != "shutdown":
        if stop == "cancel":
            c.call("RunCancelRequest", runId=second)
        state = wait_run(c, second, timeout=20).state
        assert state == (RunState.cancelled if stop == "cancel" else RunState.failed)
        inv, = c.call("RunInvocationsRequest", runId=second, card=path("resident")).records
        assert inv.workerReused
        assert inv.state == (CardState.cancelled if stop == "cancel" else CardState.failed)
        assert inv.failure.kind == (FailureKind.cancelled if stop == "cancel" else FailureKind.timeout)
    daemon.proc.send_signal(signal.SIGTERM)
    assert daemon.proc.wait(timeout=10) == 0
