"""Read a real Card's Nix output from run history."""

import re
import select
from uuid import uuid4

import pytest

from harness.file_events import process_exit
from harness.real_cards import log
from projects import card, run_and_wait, start_run, wait_for_log, wait_run, write
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

def test_stop_disconnects_the_daemon_build(core_factory, monkeypatch, tmp_path):
    monkeypatch.setenv("NIX_REMOTE", "daemon")
    root = tmp_path / "project"
    definition = {
        "language": "bash",
        "_outputs": "table Outputs {} root_type Outputs;",
        "_nix": r'''
{ pkgs, inputs, params }:
let
  probe = pkgs.runCommand "tsugumi-build-stop-@id@" {} ''
    mkfifo gate
    printf 'builder started: %s\n' "$$" >&2
    read -r line < gate
    touch "$out"
  '';
in { run = "test -f ${probe}"; }
'''.replace("@id@", uuid4().hex),
    }
    write(root, {"probe": definition}, [card("probe", "probe")], [])
    client = core_factory(project_root=root).connect()
    run_id = start_run(client, "probe", propagate=False)
    marker = re.compile(r"builder started: (\d+)")
    build = wait_for_log(client, run_id, "probe", LogStream.build,
                         lambda entry: marker.search(text(entry.text) or ""))
    pid = int(marker.search(text(build.text))[1])

    with process_exit(pid) as exited:
        assert client.call("RunCancelRequest", runId=run_id).state == RunState.cancelling
        # The upper bounds only make an uninterruptible builder fail the test.
        assert wait_run(client, run_id, timeout=15).state == RunState.cancelled
        assert select.select([exited], [], [], 15)[0], f"Nix builder {pid} is still alive"
