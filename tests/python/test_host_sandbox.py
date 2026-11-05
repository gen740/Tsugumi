"""A tsugumid on a Mac (the Web UI) isolates sandboxed Cards with Seatbelt:
a Card allowed the network resolves names but reaches nothing else on this
host (tsugumid's ports and sockets, the web server, by any of its addresses)
and nothing of the user's session (the clipboard, LaunchServices), and it
writes only its own output folders."""

from __future__ import annotations

import ctypes
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

import pytest

from conftest import Daemon
from projects import card, conn, run_and_wait, write
from test_cards_container import TXT, bash, nix_string
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(sys.platform != "darwin", reason="Seatbelt is the isolation of a tsugumid on a Mac")


def log_of(c, run_id: str, card_id: str) -> str:
    return text(c.call("LogRangeRequest", runId=run_id, card=path(card_id), stream=1, limitBytes=65536).text)


def stdout_of_network_card(tmp_path, script: str, python: str | None = None) -> str:
    """Run a script as a sandboxed Card allowed the network; what it printed.
    With `python`, that program is $PROBE in the script, $PYTHON its interpreter."""
    probe = bash(script + '\nprintf \'{"out": {"value": "done"}}\' > "$TSUGUMI_OUTPUT"', outputs=TXT, network=True)
    if python:
        probe["_nix"] = probe["_nix"].replace(
            "run = ",
            f'run = "PYTHON=${{pkgs.python3}}/bin/python3 PROBE=${{pkgs.writeText "probe.py" {nix_string(python)}}}\\n" + ',
            1)
    root = tmp_path / "p"
    write(root, {"start": {"template": "trigger"}, "probe": dict(probe, inputs=[{"name": "start"}])},
          [card("start", "start"), card("probe", "probe")], [conn("start", "out", "probe", "start")])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, st = run_and_wait(c, "start", timeout=600)
        stderr = text(c.call("LogRangeRequest", runId=run_id, card=path("probe"), stream=2, limitBytes=65536).text)
        assert st.state == RunState.succeeded, log_of(c, run_id, "probe") + stderr
        return log_of(c, run_id, "probe")
    finally:
        d.cleanup()


def accept_in_background(listener: socket.socket) -> list:
    """What connects to the listener; closing the listener ends the wait."""
    accepted = []

    def accept():
        try:
            accepted.append(listener.accept())
        except OSError:
            pass  # the listener closed at the end of the test: nothing came

    listener.listen(1)
    threading.Thread(target=accept, daemon=True).start()
    return accepted


def unix_probe(socket_path: str) -> str:
    return (f'if /usr/bin/nc -U -w 1 {socket_path} </dev/null >/dev/null 2>&1; '
            'then echo connected; else echo refused; fi')


def test_a_card_with_network_reaches_nothing_on_this_host(tmp_path):
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        accepted = accept_in_background(listener)
        port = listener.getsockname()[1]
        out = stdout_of_network_card(
            tmp_path,
            f'if (exec 3<>/dev/tcp/127.0.0.1/{port}) 2>/dev/null; then r=connected; else r=refused; fi\n'
            'echo "loopback: $r"')
        assert "loopback: refused" in out
        assert not accepted


def an_address_of_this_host() -> str:
    """The address this Mac goes out with (no packet is sent), which it answers
    on itself: a LAN or VPN address, not loopback."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("192.0.2.1", 9))
        except OSError:
            pytest.skip("this Mac has no route out")
        return s.getsockname()[0]


def test_a_card_with_network_reaches_this_host_by_none_of_its_addresses(tmp_path):
    """A service listening on 0.0.0.0 answers on every address of this Mac. To
    Seatbelt they are all "localhost", so the denial covers them too."""
    address = an_address_of_this_host()
    with socket.socket() as listener:
        listener.bind(("0.0.0.0", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        # Outside the sandbox it does answer there.
        socket.create_connection((address, port), timeout=5).close()
        listener.accept()[0].close()
        accepted = accept_in_background(listener)
        out = stdout_of_network_card(
            tmp_path,
            f'for a in {address} 0.0.0.0; do\n'
            f'  if (exec 3<>/dev/tcp/$a/{port}) 2>/dev/null; then r=connected; else r=refused; fi\n'
            '  echo "$a: $r"\n'
            'done')
        assert f"{address}: refused" in out
        assert "0.0.0.0: refused" in out
        assert not accepted


MACH_LOOKUP = """
import ctypes, sys
libsystem = ctypes.CDLL(None)
bootstrap = ctypes.c_uint.in_dll(libsystem, "bootstrap_port")
for name in sys.argv[1:]:
    port = ctypes.c_uint()
    found = libsystem.bootstrap_look_up(bootstrap, name.encode(), ctypes.byref(port)) == 0
    print(name, "reached" if found else "denied")
"""


def reached_on_this_host(names: list[str]) -> list[str]:
    libsystem = ctypes.CDLL(None)
    bootstrap = ctypes.c_uint.in_dll(libsystem, "bootstrap_port")
    return [n for n in names if libsystem.bootstrap_look_up(bootstrap, n.encode(), ctypes.byref(ctypes.c_uint())) == 0]


def test_a_card_with_network_reaches_no_mach_service_of_the_session(tmp_path):
    """Only the name of a service is looked up: nothing is read from it. A
    Card that reached the pasteboard could read the clipboard (pbpaste does)."""
    session = ["com.apple.pasteboard.1", "com.apple.coreservices.launchservicesd", "com.apple.lsd.open",
               "com.apple.lsd.mapdb", "com.apple.coreservices.appleevents", "com.apple.windowserver.active",
               "com.apple.dock.server", "com.apple.tccd", "com.apple.SecurityServer"]
    session = reached_on_this_host(session)
    if not session:
        pytest.skip("no GUI session on this host: nothing to keep the Card from")
    certificates = "com.apple.trustd.agent"
    assert reached_on_this_host([certificates])
    out = stdout_of_network_card(tmp_path, f'"$PYTHON" "$PROBE" {" ".join(session)} {certificates}', python=MACH_LOOKUP)
    for name in session:
        assert f"{name} denied" in out
    # Go and NSURLSession verify certificates through it.
    assert f"{certificates} reached" in out


def test_a_card_with_network_reaches_only_the_resolver_among_unix_sockets(tmp_path):
    """The socket of mDNSResponder is the one exception to the denial of Unix
    sockets, not a sign that local services became reachable."""
    with tempfile.TemporaryDirectory(dir="/tmp") as folder, socket.socket(socket.AF_UNIX) as listener:
        # In /tmp: a Unix socket path is limited to about 100 bytes.
        other = str(Path(folder) / "s")
        listener.bind(other)
        accepted = accept_in_background(listener)
        out = stdout_of_network_card(
            tmp_path,
            f'echo "resolver: $({unix_probe("/var/run/mDNSResponder")})"\n'
            f'echo "other: $({unix_probe(other)})"')
        assert "resolver: connected" in out
        assert "other: refused" in out
        assert not accepted


def test_a_card_with_network_resolves_a_name(tmp_path):
    """This Mac's .local name: not in /etc/hosts (as localhost is), so only
    mDNSResponder answers it, and it does so without the internet."""
    host = subprocess.run(["/usr/sbin/scutil", "--get", "LocalHostName"], capture_output=True, text=True).stdout.strip()
    name = f"{host}.local"
    try:
        socket.getaddrinfo(name, None)
    except OSError:
        pytest.skip(f"{name} does not resolve on this host either")
    out = stdout_of_network_card(
        tmp_path,
        # The discard port: what matters is the name, not the connection.
        f'm=$( (exec 3<>/dev/tcp/{name}/9) 2>&1 ) || true\n'
        'case "$m" in *nodename*) echo "name: unresolved $m";; *) echo "name: resolved";; esac')
    assert "name: resolved" in out
