"""A sandboxed Card on a host tsugumid -- bubblewrap on Linux (CI), Seatbelt
on a Mac (the Web UI) -- reaches nothing on this host: no loopback port, no
file outside its own areas, no write outside its output. These run without
the shared VM, so CI checks the isolation; the VM's own (test_cards_container.py,
test_workers.py) needs TSUGUMI_TEST_CONTAINER=1.

Writes are judged by what happened on the host. Seatbelt refuses them, while
under bubblewrap the Card's root is a private tmpfs: the same path is an empty
folder there, a write succeeds and is gone with the sandbox."""

from __future__ import annotations

import socket

from projects import card, conn, run_and_wait, write
from harness.real_cards import TXT, accept_in_background, bash
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState


def log_of(c, run_id: str, card_id: str, stream: int = 1) -> str:
    return text(c.call("LogRangeRequest", runId=run_id, card=path(card_id), stream=stream, limitBytes=65536).text)


def test_a_sandboxed_card_reaches_no_port_and_no_file_of_this_host(core_factory, tmp_path):
    """A sandboxed card reaches no port and no file of this host."""
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
        accepted = accept_in_background(listener)
        secret = tmp_path / "secret.txt"
        secret.write_text("of this host")
        escape = tmp_path / "escape.txt"
        project_file = tmp_path / "p" / "tsugumi.json"
        probe = bash(
            f'if (exec 3<>/dev/tcp/127.0.0.1/{port}) 2>/dev/null; then echo "loopback: connected"; else echo "loopback: refused"; fi\n'
            f'if cat {secret} 2>/dev/null; then echo "read: yes"; else echo "read: no"; fi\n'
            f'if cat {project_file} 2>/dev/null; then echo "project read: yes"; else echo "project read: no"; fi\n'
            f'echo x > {escape} 2>/dev/null || true\n'
            f'echo overwritten > {secret} 2>/dev/null || true\n'
            f'echo tampered >> {project_file} 2>/dev/null || true\n'
            'printf \'{"out": {"value": "done"}}\' > "$TSUGUMI_OUTPUT"', outputs=TXT)
        root = tmp_path / "p"
        write(root, {"start": {"type": "trigger"}, "probe": dict(probe, inputs=[{"name": "start"}])},
              [card("start", "start"), card("probe", "probe")], [conn("start", "out", "probe", "start")])
        d = core_factory(project_root=root)
        c = d.connect()
        run_id, st = run_and_wait(c, "start")
        log = log_of(c, run_id, "probe")
        record = c.call("RunRecordRequest", runId=run_id, card=path("probe")).status
        why = record.failure and text(record.failure.diagnostic)
        assert st.state == RunState.succeeded, (why, log, log_of(c, run_id, "probe", stream=2))
        assert "loopback: refused" in log and not accepted, log
        assert "read: no" in log and "of this host" not in log, log
        assert "project read: no" in log, log
        assert not escape.exists(), log
        assert secret.read_text() == "of this host", log
        assert "tampered" not in project_file.read_text(), log


def test_linux_switches_and_named_grants(core_factory, tmp_path):
    import os
    import sys
    import pytest

    if sys.platform != "linux":
        pytest.skip("Linux namespaces")
    shared = tmp_path / "shared"
    shared.mkdir()
    (shared / "data").write_text("readable")
    outside = tmp_path / "outside"
    outside.write_text("outside")
    original_pid = os.readlink("/proc/self/ns/pid")
    original_ipc = os.readlink("/proc/self/ns/ipc")
    scripts = {
        "disabled": bash(
            f'test "$UID" = {os.getuid()} && '
            f'test "$(readlink /proc/self/ns/pid)" = "{original_pid}" && '
            f'test "$(readlink /proc/self/ns/ipc)" = "{original_ipc}" && '
            f'test -f "{outside}" && '
            """printf '{"out":{"value":"open"}}' > "$TSUGUMI_OUTPUT" """,
            outputs=TXT, sandbox={"filesystem": False, "pid": False, "ipc": False, "identity": False}),
        "enabled": bash(
            'test "$UID" = 65534 && '
            f'test "$(readlink /proc/self/ns/pid)" != "{original_pid}" && '
            f'test "$(readlink /proc/self/ns/ipc)" != "{original_ipc}" && '
            f'test ! -f "{outside}" && '
            'test "$(cat "$TSUGUMI_GRANT_SAMPLE_DATA/data")" = readable && '
            'if echo modified >> "$TSUGUMI_GRANT_SAMPLE_DATA/data"; then exit 1; fi; '
            """printf '{"out":{"value":"isolated"}}' > "$TSUGUMI_OUTPUT" """,
            outputs=TXT, grants={"hostPaths": [{"name": "sample-data", "path": str(shared)}]}),
    }
    root = tmp_path / "p"
    write(root, scripts, [card(name, name) for name in scripts], [])
    client = core_factory(project_root=root).connect()
    for name in scripts:
        run_id, status = run_and_wait(client, name)
        assert status.state == RunState.succeeded, log_of(client, run_id, name, 2)
    assert (shared / "data").read_text() == "readable"
