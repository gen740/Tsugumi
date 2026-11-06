"""A sandboxed Card on a host tsugumid -- bubblewrap on Linux (CI), Seatbelt
on a Mac (the Web UI) -- reaches nothing on this host: no loopback port, no
file outside its own areas, no write outside its output. These run without
the shared VM, so CI checks the isolation on every change; the VM's own
(test_cards_container.py, test_workers.py) needs TSUGUMI_TEST_CONTAINER=1."""

from __future__ import annotations

import socket
import threading

from conftest import Daemon
from projects import card, conn, run_and_wait, write
from test_cards_container import TXT, bash
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState


def log_of(c, run_id: str, card_id: str) -> str:
    return text(c.call("LogRangeRequest", runId=run_id, card=path(card_id), stream=1, limitBytes=65536).text)


def test_a_sandboxed_card_reaches_no_port_and_no_file_of_this_host(tmp_path):
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    accepted = []

    def accept():
        try:
            accepted.append(listener.accept())
        except OSError:
            pass  # the listener closed at the end of the test: nothing came

    threading.Thread(target=accept, daemon=True).start()
    secret = tmp_path / "secret.txt"
    secret.write_text("of this host")
    escape = tmp_path / "escape.txt"
    probe = bash(
        f'if (exec 3<>/dev/tcp/127.0.0.1/{port}) 2>/dev/null; then echo "loopback: connected"; else echo "loopback: refused"; fi\n'
        f'if cat {secret} 2>/dev/null; then echo "read: yes"; else echo "read: no"; fi\n'
        f'if echo x > {escape} 2>/dev/null; then echo "write: yes"; else echo "write: no"; fi\n'
        'printf \'{"out": {"value": "done"}}\' > "$TSUGUMI_OUTPUT"', outputs=TXT)
    root = tmp_path / "p"
    write(root, {"start": {"template": "trigger"}, "probe": dict(probe, inputs=[{"name": "start"}])},
          [card("start", "start"), card("probe", "probe")], [conn("start", "out", "probe", "start")])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, st = run_and_wait(c, "start", timeout=600)
        log = log_of(c, run_id, "probe")
        assert st.state == RunState.succeeded, log
        assert "loopback: refused" in log and not accepted, log
        assert "read: no" in log and "of this host" not in log, log
        assert "write: no" in log and not escape.exists(), log
    finally:
        d.cleanup()
        listener.close()
