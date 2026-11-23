"""Read a real Card's Nix output from run history."""

import os
import socket
import sqlite3
from contextlib import ExitStack, closing
from uuid import uuid4

import pytest

from harness.nix_path import test_lock_entries as fixture_locks
from harness.real_cards import log, python_card, python_nix
from harness.scenario import run
from projects import (MSG_FBS, card, run_and_wait, start_run, wait_for_log, wait_run,
                      wait_run_notification, write)
from tsugumi_client import path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.RunEventValue import RunEventValue

# Cards that depend on the probe derivation through main.py's text.
PROBE_MAIN = """import os
from card import Output

def run(input):
    assert os.path.exists(PROBE)
    print(PRINTED, flush=True)
    return Output()
"""
CAT_MAIN = """from card import Output

def run(input):
    print(open(PROBE).read(), end="", flush=True)
    return Output()
"""
EXISTS_MAIN = """import os
from card import Output

def run(input):
    assert os.path.exists(PROBE)
    return Output()
"""
EMPTY_MAIN = """from card import Output

def run(input):
    return Output()
"""


@pytest.mark.parametrize(
    ("finish", "state", "truncating"),
    [
        ('touch "$out"', RunState.succeeded, False),
        ("exit 23", RunState.failed, False),
        ("exec sleep infinity", RunState.cancelled, False),
        (
            # One oversized record reaches the build client's frame limit.
            r'''{ head -c $((129 * 1024 * 1024)) /dev/zero | tr '\0' '"'; echo; } >&2; touch "$out"''',
            RunState.succeeded,
            True,
        ),
    ],
    ids=["succeeds", "fails", "cancels", "truncates"],
)
def test_build_log_preserves_output_and_flushes_before_notifications(core_factory, project, finish, state, truncating):
    cancelling = state == RunState.cancelled
    # A unique derivation forces the daemon to build and stream these records.
    definition = {
        "type": "code",
        "_outputs": "table Outputs {} root_type Outputs;",
        "_nix": r'''
{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  probe = pkgs.runCommand "tsugumi-build-log-@id@" {} ''
    printf 'before invalid bytes\n' >&2
    printf 'builder byte: \377\n' >&2
    printf 'after invalid bytes\n' >&2
    printf '%s\n' \
      '@nix {"action":"start","id":42,"level":3,"type":101,"text":"","fields":[],"parent":0}' \
      '@nix {"action":"result","id":42,"type":101,"fields":[23]}' \
      '@nix {"action":"result","id":42,"type":101}' \
      '@nix {"action":"stop","id":42}' >&2
    for ((i = 0; i < 1200; i++)); do printf 'builder line %04d\n' "$i"; done >&2
    printf 'builder reached finish\n' >&2
    @finish@
  '';
in
@run@'''.replace("@id@", uuid4().hex).replace("@finish@", finish).replace(
            "@run@", python_nix(PROBE_MAIN, values={"PROBE": "\"${probe}\"", "PRINTED": "\"card-ran\""}).split("\n", 1)[1]),
    }
    if cancelling:
        # Let the gate fail visibly if a regression prevents live log delivery.
        definition["buildTimeoutSeconds"] = 120
    write(project, {"probe": definition}, [card("probe", "probe")], [])
    client = core_factory().connect()
    run_id = run(client, "probe", propagate=False)

    def log_update(note):
        return (
            note.valueType == NotificationPayload.LogAvailable
            and text(note.value.runId) == run_id
            and note.value.invocation == 1
        )

    def finished(note):
        return (
            note.valueType == NotificationPayload.RunEvent
            and text(note.value.runId) == run_id
            and note.value.valueType == RunEventValue.RunFinished
        )

    expected_log = "before invalid bytes\nbuilder byte: \ufffd\nafter invalid bytes\n"
    expected_log += "".join(f"builder line {index:04}\n" for index in range(1200))
    expected_log += "builder reached finish\n"
    builder_prefixes = (
        "before invalid bytes", "builder byte: ", "after invalid bytes", "builder line ", "builder reached finish",
    )

    def assert_accepted_log(build):
        output = "".join(line for line in build.splitlines(keepends=True) if line.startswith(builder_prefixes))
        assert output == expected_log

    if cancelling:
        while True:
            note = client.wait_notification(lambda n: log_update(n) or finished(n))
            assert not finished(note), "build finished before cancellation"
            if "builder reached finish\n" in log(client, run_id, "probe", LogStream.build):
                client.call("RunCancelRequest", runId=run_id)
                break
    if truncating:
        note = client.wait_notification(
            lambda n: (log_update(n) and n.value.stream == LogStream.unspecified) or finished(n)
        )
        assert not finished(note), "build finished without announcing truncation"
        live = client.call("LogRangeRequest", runId=run_id, card=path("probe"), invocation=1,
                           stream=LogStream.build, limitBytes=65536)
        assert live.logTruncatedAtBytes == 128 << 20
        assert_accepted_log(text(live.text))
        assert live.sizeBytes < 65536

    status = wait_run_notification(client, run_id)
    build = log(client, run_id, "probe", LogStream.build)
    record = client.call("RunRecordRequest", runId=run_id, card=path("probe"), invocation=1).record

    assert status.state == state, (record.summary.failure, build)
    assert_accepted_log(build)
    assert build.split("after invalid bytes\n", 1)[1].split("builder line 0000\n", 1)[0].strip()
    with closing(sqlite3.connect(project / ".tsugumi" / "runs.sqlite")) as db:
        writes, = db.execute("SELECT count(*) FROM logs WHERE run_id=? AND stream=?",
                             (run_id, LogStream.build)).fetchone()
    # The reader collects lines across pipe reads and delivers them on an
    # interval, so writes follow elapsed time, not how the OS splits reads
    # (a read often holds one or two lines). 1,200 lines must not
    # become one write per line or per read.
    assert writes < 600

    if state == RunState.failed:
        assert record.summary.state == CardState.failed
        assert record.summary.failure.kind == FailureKind.nix_failed
    if state == RunState.succeeded and not truncating:
        assert log(client, run_id, "probe", LogStream.standard_output) == "card-ran\n"


@pytest.mark.parametrize(
    ("max_jobs", "options", "expected"),
    [
        ("1", {}, "2\n"),
        ("0", {}, None),
        ("0", {"nixJobs": 1, "nixCores": 1}, "1\n"),
    ],
    ids=["defaults", "local-builds-disabled", "explicit-jobs-and-cores"],
)
def test_run_nix_parallelism(core_factory, tmp_path, monkeypatch, max_jobs, options, expected):
    config = os.environ.get("NIX_CONFIG", "")
    monkeypatch.setenv("NIX_CONFIG", f"{config}\nmax-jobs = {max_jobs}\ncores = 2\nbuilders =")
    root = tmp_path / "project"
    definition = {
        "type": "code",
        "_outputs": "table Outputs {} root_type Outputs;",
        "_nix": r'''
{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  probe = pkgs.runCommand "tsugumi-build-cores-@id@" {} ''
    printf '%s\n' "$NIX_BUILD_CORES" > "$out"
  '';
in
@run@'''.replace("@id@", uuid4().hex).replace(
            "@run@", python_nix(CAT_MAIN, values={"PROBE": "\"${probe}\""}).split("\n", 1)[1]),
    }
    write(root, {"probe": definition}, [card("probe", "probe")], [])
    client = core_factory(project_root=root).connect()
    run_id = run(client, "probe", propagate=False, **options)
    outcome = wait_run(client, run_id)
    if expected is None:
        record = client.call("RunRecordRequest", runId=run_id, card=path("probe"), invocation=1).record
        assert outcome.state == RunState.failed
        assert record.summary.failure.kind == FailureKind.nix_failed
    else:
        assert outcome.state == RunState.succeeded, log(client, run_id, "probe", LogStream.build)
        assert log(client, run_id, "probe", LogStream.standard_output) == expected

def test_stop_disconnects_the_daemon_build(core_factory, monkeypatch, tmp_path):
    monkeypatch.setenv("NIX_REMOTE", "daemon")
    root = tmp_path / "project"
    with ExitStack() as stack:
        listener = stack.enter_context(socket.socket())
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        # Bounds for reporting a missing fixture event or a deadlock.
        listener.settimeout(120)
        port = listener.getsockname()[1]
        defs = {"probe": blocked_builder("probe", port)}
        defs["warm"] = python_card(EMPTY_MAIN, outputs="table Outputs {} root_type Outputs;")
        write(root, defs, [card(name, name) for name in defs], [])
        client = core_factory(project_root=root).connect()
        # Exercise a build on a later Run, using the existing evaluator.
        _, warm = run_and_wait(client, "warm", propagate=False)
        assert warm.state == RunState.succeeded
        run_id = start_run(client, "probe", propagate=False)

        peer, _ = listener.accept()
        stack.enter_context(peer)
        peer.settimeout(15)
        marker = stack.enter_context(peer.makefile("rb"))
        assert marker.readline().decode().strip() == "probe"
        wait_for_log(client, run_id, "probe", LogStream.build,
                     lambda entry: "builder started" in (text(entry.text) or ""))

        assert client.call("RunCancelRequest", runId=run_id).state == RunState.cancelling
        assert wait_run(client, run_id, timeout=15).state == RunState.cancelled
        # The blocked builder holds its socket open until the daemon ends it.
        # This observes its exit independently of the core's Run status.
        assert marker.read(1) == b"", "the builder is still alive"


def test_stop_ends_locked_source_materialization(core_factory, monkeypatch, tmp_path):
    root = tmp_path / "project"
    with ExitStack() as stack:
        proxy = stack.enter_context(socket.socket())
        proxy.bind(("127.0.0.1", 0))
        proxy.listen()
        proxy.settimeout(120)
        address = "http://127.0.0.1:" + str(proxy.getsockname()[1])

        # The pinned NAR is absent, so materializing the lock uses this proxy.
        pin = next(entry for entry in fixture_locks() if entry["name"] == "nixpkgs")
        revision = uuid4().hex + "cafef00d"
        pin["locked"] = dict(pin["locked"], rev=revision,
                             narHash="sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=")
        write(root, {"fetch": {"type": "code", "_nix": "{ params, inputs }: { run = []; }"}},
              [card("fetch", "fetch")], [], locked=[pin])
        # This fixture never answers. Its network deadlines must not end the
        # wait on behalf of Stop (the host may configure a short connect timeout).
        monkeypatch.setenv("NIX_CONFIG", os.environ.get("NIX_CONFIG", "")
                           + "\nconnect-timeout = 0\nstalled-download-timeout = 0")
        monkeypatch.setenv("https_proxy", address)
        monkeypatch.setenv("no_proxy", "")
        client = core_factory(project_root=root).connect()
        run_id = start_run(client, "fetch", propagate=False)

        # This proxy keeps the evaluator in libnix's transfer wait.
        peer, _ = proxy.accept()
        stack.enter_context(peer)
        peer.settimeout(15)
        stream = stack.enter_context(peer.makefile("rb"))
        connect = stream.readline()
        assert connect.startswith(b"CONNECT ") and b"github.com:443 " in connect
        while stream.readline().strip():
            pass
        assert client.call("RunCancelRequest", runId=run_id).state == RunState.cancelling
        assert wait_run(client, run_id, timeout=15).state == RunState.cancelled
        assert stream.read(1) == b"", "the stopped evaluator still has an open fetch"


def blocked_builder(name, port):
    return {
        "type": "code",
        "_outputs": MSG_FBS,
        # Fixed-output derivations can reach loopback in the Nix sandbox.
        # The fixture checks the connection lifetime across PID namespaces.
        "_nix": r'''
{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  probe = pkgs.runCommand "tsugumi-build-stop-@id@" {
    outputHashMode = "flat";
    outputHashAlgo = "sha256";
    outputHash = builtins.hashString "sha256" "";
  } ''
    exec 3<>/dev/tcp/127.0.0.1/@port@
    printf '@card@\n' >&3
    printf 'builder started\n' >&2
    read -r line <&3
    touch "$out"
  '';
in
@run@'''.replace("@id@", uuid4().hex).replace("@port@", str(port)).replace("@card@", name).replace(
            "@run@", python_nix(EXISTS_MAIN, values={"PROBE": "\"${probe}\""}).split("\n", 1)[1]),
    }
