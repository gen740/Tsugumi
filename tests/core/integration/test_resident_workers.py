"""Resident workers keep Card state across successful calls and lease handover."""

from pathlib import Path
import signal

import pytest

from harness.file_events import wait_for_file
from harness.observations import decode_scalar, read_output
from harness.real_cards import python_card, log
from harness.scenario import Project
from projects import card, conn, run_and_wait, start_run, wait_run, write_nix_project as write
from test_worker_loading import DIRECTORY, native_card
from tsugumi_client import path
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


COUNT = "static unsigned count = 0; out.number(++count);"


def test_more_than_32_residents_keep_their_state(core_factory, tmp_path):
    """A 33rd resident does not evict the first Card's process."""
    definition = native_card(COUNT, reuseWorker=True, recordIO="on")
    names = [f"c{i}" for i in range(33)]
    write(tmp_path / "p", {"resident": definition},
          [card(name, "resident") for name in names], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    seen = set()
    for name in [*names, "c0"]:
        run_id, state = run_and_wait(c, name)
        assert state.state == RunState.succeeded, log(c, run_id, name, 2)
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path(name)).records
        assert inv.workerReused == (name in seen)
        assert decode_scalar(read_output(c, run_id, card=name), "uint64") == (2 if name in seen else 1)
        seen.add(name)


def test_resident_definition_change_records_fresh_state(core_factory, tmp_path):
    root = tmp_path / "p"
    write(root, {}, [], [])
    c = core_factory(project_root=root).connect()
    graph = Project(root)
    for greeting, count, reused in [("first", 1, False), ("first", 2, True), ("changed", 1, False)]:
        definition = native_card(f'puts("{greeting}"); ' + COUNT)
        graph.card("resident", {"type": "code", "reuseWorker": True, "recordIO": "on"},
                   outputs=definition["_outputs"], sources={"default.nix": definition["_nix"]})
        graph.save(c)
        run_id, state = run_and_wait(c, "resident")
        assert state.state == RunState.succeeded, log(c, run_id, "resident", 2)
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("resident")).records
        assert inv.workerReused == reused
        assert decode_scalar(read_output(c, run_id, card="resident"), "uint64") == count
        assert greeting in log(c, run_id, "resident")
        if greeting == "changed":
            assert "state started fresh: the Card definition changed" in log(c, run_id, "resident", 2)
        else:
            assert "state started fresh" not in log(c, run_id, "resident", 2)


@pytest.mark.parametrize("crashes", [True, False])
def test_failed_resident_is_discarded(core_factory, tmp_path, crashes):
    body = ('kill(getpid(), SIGKILL);' if crashes else
            'static bool failed; if (!failed) { failed = true; return 19; } out.number(1);')
    definition = native_card(body, reuseWorker=True)
    write(tmp_path / "p", {"resident": definition}, [card("resident", "resident")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    for call in (1, 2):
        run_id, state = run_and_wait(c, "resident")
        assert state.state == RunState.failed
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("resident")).records
        assert not inv.workerReused
        if call == 2:
            reason = "the resident worker died" if crashes else "the previous call failed"
            assert f"state started fresh: {reason}" in log(c, run_id, "resident", 2)


def test_artifact_creation_retires_resident_even_when_transfer_fails(core_factory, tmp_path):
    definition = native_card(r"""
        static unsigned calls;
        tsugumi_artifact_id id;
        const auto file = fs::path(create_artifact(call, id)) / "result";
        if (++calls != 1) return 1;
        if (mkfifo(file.c_str(), 0600)) return 2;
        out.artifact(id);
    """, outputs=DIRECTORY, reuseWorker=True)
    write(tmp_path / "p", {"resident": definition}, [card("resident", "resident")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    for _ in range(2):
        run_id, state = run_and_wait(c, "resident")
        assert state.state == RunState.failed
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("resident")).records
        assert inv.failure.kind == FailureKind.output_invalid
        assert not inv.workerReused


def test_missing_required_output_discards_resident_state(core_factory, tmp_path):
    definition = native_card(r'''
        static unsigned calls;
        if (++calls == 2) out.number(2);
    ''', reuseWorker=True, recordIO="on")
    write(tmp_path / "p", {"resident": definition}, [card("resident", "resident")], [])
    c = core_factory(project_root=tmp_path / "p").connect()
    for _ in range(2):
        run_id, state = run_and_wait(c, "resident")
        assert state.state == RunState.failed, log(c, run_id, "resident", 2)
        inv, = c.call("RunInvocationsRequest", runId=run_id, card=path("resident")).records
        assert inv.failure.kind == FailureKind.output_invalid
        assert {a.key: a.value for a in inv.failure.args}[b"port"] == b"out"
        assert not inv.workerReused


@pytest.mark.parametrize("stop", ["cancel", "deadline", "shutdown"])
def test_stopped_resident_can_interrupt_a_large_send(core_factory, tmp_path, stop):
    """A stopped resident cannot keep the next invocation blocked in send."""
    definition = native_card(r'''
        if (mkfifo("stop", 0600)) return 1;
        const auto worker = getpid();
        const auto child = fork();
        if (child < 0) return 1;
        if (!child) {
          const int release = open("stop", O_RDONLY);
          if (release < 0) _exit(4);
          close(release);
          if (kill(worker, SIGSTOP) != 0) _exit(2);
          close(open("stopped", O_WRONLY | O_CREAT, 0600));
          // Observe the second call without draining it so the sender blocks.
          struct pollfd worker_data = {3, POLLIN, 0};
          char byte;
          if (poll(&worker_data, 1, -1) != 1 ||
              recv(3, &byte, sizeof(byte), MSG_PEEK) != 1)
            _exit(3);
          close(open("sending", O_WRONLY | O_CREAT, 0600));
          _exit(0);
        }
        out.number(1);
        printf("%s/stop\n", std::getenv("HOME"));
    ''', declarations="#include <sys/socket.h>", inputs=["data"],
        reuseWorker=True, recordIO="on")
    if stop == "deadline":
        definition["timeoutSeconds"] = 5
    payload = python_card("""
from card import Output
from Text import TextT


def run(input):
    return Output(data=TextT(value="x" * 4194304))
""", outputs="table Text { value:string; } table Outputs { data:Text (required); } root_type Outputs;")
    root = tmp_path / "p"
    write(root, {"payload": payload, "resident": definition},
          [card("source", "payload"), card("resident", "resident")],
          [conn("source", "data", "resident", "data")])
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    first, state = run_and_wait(c, "source")
    assert state.state == RunState.succeeded, log(c, first, "resident", 2)
    gate = Path(log(c, first, "resident").strip())
    with gate.open("w"):
        pass
    wait_for_file(gate.with_name("stopped"))
    second = start_run(c, "source")
    # The first invocation's child can only see this marker after it has
    # observed the next call on the same resident's socket.
    wait_for_file(gate.with_name("sending"))
    if stop != "shutdown":
        if stop == "cancel":
            c.call("RunCancelRequest", runId=second)
        state = wait_run(c, second).state
        assert state == (RunState.cancelled if stop == "cancel" else RunState.failed)
        inv, = c.call("RunInvocationsRequest", runId=second, card=path("resident")).records
        assert inv.workerReused
        expected = CardState.cancelled if stop == "cancel" else CardState.failed
        assert inv.state == expected, (
            inv.state, vars(inv.failure) if inv.failure else None,
            log(c, second, "resident", 2)
        )
        assert inv.failure.kind == (FailureKind.cancelled if stop == "cancel" else FailureKind.timeout)
    daemon.proc.send_signal(signal.SIGTERM)
    assert daemon.proc.wait() == 0
