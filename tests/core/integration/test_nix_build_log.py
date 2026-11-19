"""Read a real Card's Nix output from run history."""

from uuid import uuid4

import pytest

from harness.real_cards import log
from projects import card, run_and_wait, write
from tsugumi_client import path
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
    import os

    from projects import wait_run
    from tsugumi_client import new_id, text

    config = os.environ.get("NIX_CONFIG", "")
    monkeypatch.setenv("NIX_CONFIG", f"{config}\nmax-jobs = {max_jobs}\ncores = 2\nbuilders =")
    root = tmp_path / "project"
    definition = {
        "language": "bash",
        "_outputs": "table Outputs {} root_type Outputs;",
        "_nix": r'''
{ pkgs, inputs, params }:
let
  probe = pkgs.runCommand "tsugumi-build-cores-@id@" {} ''
    printf '%s\n' "$NIX_BUILD_CORES" > "$out"
  '';
in {
  run = "cat ${probe}";
}
'''.replace("@id@", uuid4().hex),
    }
    write(root, {"probe": definition}, [card("probe", "probe")], [])
    client = core_factory(project_root=root).connect()
    rev = client.call("ProjectInspectRequest").rev
    accepted = client.call("RunRequest", baseRev=rev, opId=new_id(), origin="test",
                           card=path("probe"), propagate=False, **options)
    run_id = text(accepted.run.runId)
    outcome = wait_run(client, run_id)
    if expected is None:
        record = client.call("RunRecordRequest", runId=run_id, card=path("probe"), invocation=1).record
        assert outcome.state == RunState.failed
        assert record.summary.failure.kind == FailureKind.nix_failed
    else:
        assert outcome.state == RunState.succeeded, log(client, run_id, "probe", LogStream.build)
        assert log(client, run_id, "probe", LogStream.standard_output) == expected
