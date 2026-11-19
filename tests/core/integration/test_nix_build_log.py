"""Read a real Card's Nix output from run history."""

import os
import socket
from contextlib import ExitStack
from uuid import uuid4

import pytest

from harness.real_cards import log
from projects import MSG_FBS, card, conn, run_and_wait, start_run, wait_for_log, wait_run, write
from tsugumi_client import path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState


@pytest.mark.parametrize("build_fails", [False, True], ids=["succeeds", "fails"])
def test_build_log_keeps_output_after_invalid_utf8(core_factory, tmp_path, build_fails):
    root = tmp_path / "project"
    # A fresh derivation keeps Nix from reusing an earlier result.
    definition = {
        "language": "bash",
        "_outputs": "table Outputs {} root_type Outputs;",
        "_nix": r'''
{ pkgs, inputs, params }:
let
  probe = pkgs.runCommand "tsugumi-build-log-@id@" {} ''
    printf 'before invalid bytes\n' >&2
    printf 'builder byte: \377\n' >&2
    printf 'after invalid bytes\n' >&2
    @finish@
  '';
in {
  preRun = ''printf 'pre-run: \377\n' >&2'';
  run = "test -f ${probe} && echo card-ran";
  postRun = ''printf 'post-run: \377\n' >&2'';
}
'''.replace("@id@", uuid4().hex).replace("@finish@", "exit 23" if build_fails else 'touch "$out"'),
    }
    write(root, {"probe": definition}, [card("probe", "probe")], [])
    client = core_factory(project_root=root).connect()
    run_id, status = run_and_wait(client, "probe", propagate=False)
    build = log(client, run_id, "probe", LogStream.build)
    record = client.call("RunRecordRequest", runId=run_id, card=path("probe"), invocation=1).record

    expected = RunState.failed if build_fails else RunState.succeeded
    assert status.state == expected, (record.summary.failure, build)
    markers = ["before invalid bytes", "builder byte: \ufffd", "after invalid bytes"]
    remaining = build
    for marker in markers:
        _, found, remaining = remaining.partition(marker)
        assert found, build
    if build_fails:
        assert record.summary.state == CardState.failed
        assert record.summary.failure.kind == FailureKind.nix_failed
    else:
        assert log(client, run_id, "probe", LogStream.standard_output) == "card-ran\n"
        stderr = log(client, run_id, "probe", LogStream.standard_error)
        assert "pre-run: \ufffd\n" in stderr
        assert "post-run: \ufffd\n" in stderr

@pytest.mark.parametrize("early_card", [False, True], ids=["activation", "unclaimed-early-build"])
def test_stop_disconnects_the_daemon_build(core_factory, monkeypatch, tmp_path, early_card):
    monkeypatch.setenv("NIX_REMOTE", "daemon")
    monkeypatch.setenv("NIX_CONFIG", os.environ.get("NIX_CONFIG", "") + "\nmax-jobs = 2")
    root = tmp_path / "project"
    with ExitStack() as stack:
        listener = stack.enter_context(socket.socket())
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        # Bounds for reporting a missing fixture event or a deadlock.
        listener.settimeout(120)
        port = listener.getsockname()[1]
        names = ["probe", "early"] if early_card else ["probe"]
        defs = {name: blocked_builder(name, port) for name in names}
        edges = []
        if early_card:
            # The early build runs while its Card is still waiting for input
            # from the blocked probe. No activation will take over its build.
            defs["early"]["inputs"] = [{"name": "in"}]
            edges.append(conn("probe", "msg", "early", "in"))
        write(root, defs, [card(name, name) for name in names], edges)
        client = core_factory(project_root=root).connect()
        run_id = start_run(client, "probe", propagate=early_card)

        builders = {}
        for _ in names:
            peer, _ = listener.accept()
            stack.enter_context(peer)
            peer.settimeout(15)
            marker = stack.enter_context(peer.makefile("rb"))
            name = marker.readline().decode().strip()
            builders[name] = marker
        assert builders.keys() == set(names)
        wait_for_log(client, run_id, "probe", LogStream.build,
                     lambda entry: "builder started" in (text(entry.text) or ""))

        assert client.call("RunCancelRequest", runId=run_id).state == RunState.cancelling
        assert wait_run(client, run_id, timeout=15).state == RunState.cancelled
        # Each blocked builder holds its socket open until the daemon ends it.
        # This observes its exit independently of the core's Run status.
        for name, marker in builders.items():
            assert marker.read(1) == b"", f"the {name} builder is still alive"


def blocked_builder(name, port):
    return {
        "language": "bash",
        "_outputs": MSG_FBS,
        # Fixed-output derivations can reach loopback in the Nix sandbox.
        # The fixture checks the connection lifetime across PID namespaces.
        "_nix": r'''
{ pkgs, inputs, params }:
let
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
in { run = "test -f ${probe}"; }
'''.replace("@id@", uuid4().hex).replace("@port@", str(port)).replace("@card@", name),
    }
