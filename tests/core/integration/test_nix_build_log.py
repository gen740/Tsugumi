"""Read a real Card's Nix output from run history."""

import os
import sqlite3
from contextlib import closing
from uuid import uuid4

import pytest

from harness.real_cards import log
from harness.scenario import run, wait_run
from projects import card, wait_run_notification, write
from tsugumi_client import path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.RunEventValue import RunEventValue


@pytest.mark.parametrize(
    ("finish", "state", "truncating"),
    [
        ('touch "$out"', RunState.succeeded, False),
        ("exit 23", RunState.failed, False),
        ("exec sleep infinity", RunState.cancelled, False),
        (
            # Quotes expand to two bytes in the pipe's JSON string. Exercise
            # the reader's bound on a record before it reaches the log budget.
            r'''{ head -c $((65 * 1024 * 1024)) /dev/zero | tr '\0' '"'; echo; } >&2; touch "$out"''',
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
        "language": "bash",
        "_outputs": "table Outputs {} root_type Outputs;",
        "_nix": r'''
{ pkgs, inputs, params }:
let
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
in {
  preRun = ''printf 'pre-run: \377\n' >&2'';
  run = "test -f ${probe} && echo card-ran";
  postRun = ''printf 'post-run: \377\n' >&2'';
}
'''.replace("@id@", uuid4().hex).replace("@finish@", finish),
    }
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
    # Pipe reads can end anywhere. Require substantially fewer writes than
    # complete records without asserting a particular split.
    assert writes < 600

    if state == RunState.failed:
        assert record.summary.state == CardState.failed
        assert record.summary.failure.kind == FailureKind.nix_failed
    if state == RunState.succeeded and not truncating:
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
    run_id = run(client, "probe", propagate=False, **options)
    outcome = wait_run(client, run_id)
    if expected is None:
        record = client.call("RunRecordRequest", runId=run_id, card=path("probe"), invocation=1).record
        assert outcome.state == RunState.failed
        assert record.summary.failure.kind == FailureKind.nix_failed
    else:
        assert outcome.state == RunState.succeeded, log(client, run_id, "probe", LogStream.build)
        assert log(client, run_id, "probe", LogStream.standard_output) == expected
