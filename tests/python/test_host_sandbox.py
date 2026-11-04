"""A tsugumid on a Mac (the Web UI) isolates sandboxed Cards with Seatbelt:
even a Card allowed the network reaches nothing on this host (tsugumid's
ports, the web server), and it writes only its own output folders."""

from __future__ import annotations

import socket
import sys
import threading

import pytest

from conftest import Daemon
from projects import card, conn, run_and_wait, write
from test_cards_container import TXT, bash
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(sys.platform != "darwin", reason="Seatbelt is the isolation of a tsugumid on a Mac")


def log_of(c, run_id: str, card_id: str) -> str:
    return text(c.call("LogRangeRequest", runId=run_id, card=path(card_id), stream=1, limitBytes=65536).text)


def test_a_card_with_network_reaches_nothing_on_this_host(tmp_path):
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
    probe = bash(
        f'if (exec 3<>/dev/tcp/127.0.0.1/{port}) 2>/dev/null; then r=connected; else r=refused; fi\n'
        'echo "loopback: $r"\n'
        'printf \'{"out": {"value": "%s"}}\' "$r" > "$TSUGUMI_OUTPUT"', outputs=TXT, network=True)
    root = tmp_path / "p"
    write(root, {"start": {"template": "trigger"}, "probe": dict(probe, inputs=[{"name": "start"}])},
          [card("start", "start"), card("probe", "probe")], [conn("start", "out", "probe", "start")])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, st = run_and_wait(c, "start", timeout=600)
        assert st.state == RunState.succeeded, log_of(c, run_id, "probe")
        assert "loopback: refused" in log_of(c, run_id, "probe")
        assert not accepted
    finally:
        d.cleanup()
        listener.close()
