"""A tsugumid on a Mac (the Web UI) isolates sandboxed Cards with Seatbelt:
a Card allowed the network resolves names but reaches nothing else on this
host (tsugumid's ports and sockets, the web server, by any of its addresses)
and nothing of the user's session (the clipboard, LaunchServices), and it
writes only its own output folders. It can run only the tools it declared,
never /bin or /usr/bin. A Python Card works there as anywhere: it can ask for
its directory while it loads, and its environment is the process's, kept
across the calls of a reused worker."""

from __future__ import annotations

import ctypes
import json
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

from projects import card, conn, run_and_wait, write_nix_project as write
from harness.observations import decode_scalar, read_output
from harness.real_cards import (ENVIRONMENT_MAIN, TXT, accept_in_background, check_process_environment,
                                python_card, txt_main)
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(sys.platform != "darwin", reason="Seatbelt is the isolation of a tsugumid on a Mac")


def log_of(c, run_id: str, card_id: str) -> str:
    return text(c.call("LogRangeRequest", runId=run_id, card=path(card_id), stream=1, limitBytes=65536).text)


def stdout_of_network_card(core_factory, tmp_path, body: str) -> str:
    """Run `body` (Python, indented by 4) as a sandboxed Card allowed the network; what it printed."""
    return stdout_of_card(core_factory, tmp_path, body, grants={"network": True})


def stdout_of_card(core_factory, tmp_path, body: str, *, tools: str = "", **settings) -> str:
    """Run `body` (Python, indented by 4) as a sandboxed Card; what it printed."""
    probe = python_card(txt_main(body + "\n    RESULT = 'done'"), outputs=TXT, tools=tools, **settings)
    root = tmp_path / "p"
    write(root, {"start": {"type": "trigger"}, "probe": dict(probe, inputs=[{"name": "start"}])},
          [card("start", "start"), card("probe", "probe")], [conn("start", "out", "probe", "start")])
    d = core_factory(project_root=root)
    c = d.connect()
    run_id, st = run_and_wait(c, "start")
    stderr = text(c.call("LogRangeRequest", runId=run_id, card=path("probe"), stream=2, limitBytes=65536).text)
    assert st.state == RunState.succeeded, log_of(c, run_id, "probe") + stderr
    return log_of(c, run_id, "probe")


def connects(target: str) -> str:
    """Python lines printing `<label>: connected|refused` for a TCP (host, port) or Unix path."""
    return f"""
    import socket
    def reach(label, address):
        family = socket.AF_UNIX if isinstance(address, str) else socket.AF_INET
        with socket.socket(family) as s:
            s.settimeout(2)
            try:
                s.connect(address)
                print(label + ": connected", flush=True)
            except OSError:
                print(label + ": refused", flush=True)
{target}"""


def test_a_card_with_network_reaches_nothing_on_this_host(core_factory, tmp_path):
    """A card with network reaches nothing on this host."""
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        accepted = accept_in_background(listener)
        port = listener.getsockname()[1]
        out = stdout_of_network_card(
            core_factory, tmp_path,
            connects(f'    reach("loopback", ("127.0.0.1", {port}))'))
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


def test_a_card_with_network_reaches_this_host_by_none_of_its_addresses(core_factory, tmp_path):
    """A network-enabled card cannot connect to any address of the host.

    A service listening on 0.0.0.0 answers on each host address outside the
    sandbox, while Seatbelt denies those same destinations inside it.
    """
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
            core_factory, tmp_path,
            connects(f'    for a in ({address!r}, "0.0.0.0"):\n        reach(a, (a, {port}))'))
        assert f"{address}: refused" in out
        assert "0.0.0.0: refused" in out
        assert not accepted


MACH_LOOKUP = """
    import ctypes
    libsystem = ctypes.CDLL(None)
    bootstrap = ctypes.c_uint.in_dll(libsystem, "bootstrap_port")
    for name in NAMES:
        port = ctypes.c_uint()
        found = libsystem.bootstrap_look_up(bootstrap, name.encode(), ctypes.byref(port)) == 0
        print(name, "reached" if found else "denied", flush=True)"""


def reached_on_this_host(names: list[str]) -> list[str]:
    libsystem = ctypes.CDLL(None)
    bootstrap = ctypes.c_uint.in_dll(libsystem, "bootstrap_port")
    return [n for n in names if libsystem.bootstrap_look_up(bootstrap, n.encode(), ctypes.byref(ctypes.c_uint())) == 0]


def test_a_card_with_network_reaches_no_mach_service_of_the_session(core_factory, tmp_path):
    """A network-enabled card cannot look up the user's session services.

    Only service names are queried; the test never reads their contents.
    The certificate service remains available for network TLS verification.
    """
    session = ["com.apple.pasteboard.1", "com.apple.coreservices.launchservicesd", "com.apple.lsd.open",
               "com.apple.lsd.mapdb", "com.apple.coreservices.appleevents", "com.apple.windowserver.active",
               "com.apple.dock.server", "com.apple.tccd", "com.apple.SecurityServer"]
    session = reached_on_this_host(session)
    if not session:
        pytest.skip("no GUI session on this host: nothing to keep the Card from")
    certificates = "com.apple.trustd.agent"
    assert reached_on_this_host([certificates])
    out = stdout_of_network_card(core_factory, tmp_path, f"\n    NAMES = {session + [certificates]!r}" + MACH_LOOKUP)
    for name in session:
        assert f"{name} denied" in out
    # Go and NSURLSession verify certificates through it.
    assert f"{certificates} reached" in out


def test_a_card_with_network_reaches_only_the_resolver_among_unix_sockets(core_factory, tmp_path):
    """A network-enabled card reaches the resolver socket while other Unix sockets remain blocked."""
    with tempfile.TemporaryDirectory() as folder, socket.socket(socket.AF_UNIX) as listener:
        # In /tmp: a Unix socket path is limited to about 100 bytes.
        other = str(Path(folder) / "s")
        listener.bind(other)
        accepted = accept_in_background(listener)
        out = stdout_of_network_card(
            core_factory, tmp_path,
            connects(f'    reach("resolver", "/var/run/mDNSResponder")\n    reach("other", {other!r})'))
        assert "resolver: connected" in out
        assert "other: refused" in out
        assert not accepted


def test_a_card_with_network_resolves_a_name(core_factory, tmp_path):
    """A network-enabled card resolves the host's local name through mDNSResponder."""
    host = subprocess.run(["/usr/sbin/scutil", "--get", "LocalHostName"], capture_output=True, text=True).stdout.strip()
    name = f"{host}.local"
    try:
        socket.getaddrinfo(name, None)
    except OSError:
        pytest.skip(f"{name} does not resolve on this host either")
    out = stdout_of_network_card(
        core_factory, tmp_path,
        # What matters is the name, not a connection.
        f"""
    import socket
    try:
        socket.getaddrinfo({name!r}, 9)
        print("name: resolved", flush=True)
    except OSError as e:
        print("name: unresolved", e, flush=True)""")
    assert "name: resolved" in out


SYSTEM_TOOLS = """
    import shutil, subprocess
    for tool in ("/bin/sh", "/usr/bin/env"):
        try:
            with open(tool, "rb") as f:
                f.read(1)
            print(tool, "read", flush=True)
        except OSError:
            print(tool, "unreadable", flush=True)
        try:
            subprocess.run([tool, "-c", "true"], stdin=subprocess.DEVNULL, capture_output=True)
            print(tool, "ran", flush=True)
        except OSError:
            print(tool, "not run", flush=True)
    jq = shutil.which("jq")
    version = subprocess.run([jq, "--version"], capture_output=True, text=True, check=True).stdout
    print("declared:", jq.startswith("/nix/store/") and version.startswith("jq-"), flush=True)"""


def test_a_sandboxed_card_runs_only_what_it_declared(core_factory, tmp_path):
    """Seatbelt gives the Card no system shell or tools: /bin and /usr/bin can be
    neither read nor run, while the tool it declared to its builder runs."""
    out = stdout_of_card(core_factory, tmp_path, SYSTEM_TOOLS, tools="[ pkgs.jq ]")
    for tool in ("/bin/sh", "/usr/bin/env"):
        assert f"{tool} unreadable" in out and f"{tool} not run" in out, out
    assert "declared: True" in out, out


def test_a_reused_worker_keeps_its_process_environment(core_factory, tmp_path):
    """What the module's top set reaches run through os.environ and the C
    environment, and what a run changes is still there in the next run of
    the reused worker: nothing is reset per call."""
    root = tmp_path / "p"
    show = python_card(ENVIRONMENT_MAIN, outputs=TXT, inputs=["start"], reuseWorker=True, recordIO="on")
    write(root, {"start": {"type": "trigger"}, "show": show},
          [card("start", "start"), card("show", "show")], [conn("start", "out", "show", "start")])
    c = core_factory(project_root=root).connect()
    seen = []
    for _ in range(2):
        run_id, st = run_and_wait(c, "start")
        assert st.state == RunState.succeeded, log_of(c, run_id, "show")
        seen.append(json.loads(decode_scalar(read_output(c, run_id, card="show"))))
    reused, = c.call("RunInvocationsRequest", runId=run_id, card=path("show")).records
    assert reused.workerReused
    check_process_environment(*seen)
