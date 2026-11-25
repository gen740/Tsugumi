"""End-to-end checks of the bundled demos against the expected results.

Run in the shared VM with TSUGUMI_TEST_SHARED_VM=1 (the reference
configuration); without it a host tsugumid is used.
"""

from __future__ import annotations

import csv
import io
import json
import os
import shutil
import struct

import pytest

from conftest import ROOT, Daemon, demo_environment, write_project
from harness.observations import read_pages
from harness.values import reference_json
from harness.views import ZERO_ROUTE, return_credit
from projects import start_run, wait_for_display, wait_invocation, wait_run
from harness.document import set_param as set_typed_param
from tsugumi_client import change, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.Trace.Client import Client

def connect(env, *, client=0):
    if getattr(env, "client_hello", False):
        return env.connect(client=client)
    return env.client(client=client)


def recorded_output(tmp_path, client, run_id, *card, port):
    record = client.call("RunRecordRequest", runId=run_id, card=path(*card)).record
    output = next(row for row in record.outputs if text(row.name) == port)
    return reference_json(tmp_path, bytes(output.schemaBfbs), bytes(output.inlineBytes),
                          root_type=text(output.rootType))


def artifact_member(client, run_id, card, member):
    artifacts = client.call("ArtifactListRequest", runId=run_id, card=path(card)).artifacts
    artifact, = (row for row in artifacts if any(text(m.path) == member for m in row.members))
    return read_pages(client, "ArtifactReadRequest", artifactId=text(artifact.artifactId),
                      member=member, page_bytes=65536)


def stderr(c, run_id, *card):
    return text(c.call("LogRangeRequest", runId=run_id, card=path(*card), stream=2, limitBytes=262144).text)


def test_every_listed_demo_resolves(tmp_path):
    """Resolve each bundled project through the same core session."""
    project = tmp_path / "demo"
    write_project(project)
    daemon = Daemon(project).start()
    try:
        client = daemon.connect()
        rev = client.call("ProjectInspectRequest").rev
        for language in ("ja", "en"):
            demos = ROOT / "demos" / language
            for entry in json.loads((demos / "index.json").read_text())["demos"]:
                demo = entry["path"]
                for child in project.iterdir():
                    if not child.name.startswith("."):
                        if child.is_dir():
                            shutil.rmtree(child)
                        else:
                            child.unlink()
                shutil.copytree(demos / demo, project, dirs_exist_ok=True,
                                ignore=shutil.ignore_patterns(".tsugumi-history"))
                rev = client.call("ProjectReloadRequest", baseRev=rev, opId=new_id(), origin="test").rev
                snapshot = client.call("ProjectInspectRequest")
                assert snapshot.resolution, (language, demo)
                codes = {text(d.code) for row in snapshot.resolution for d in row.diagnostics}
                assert codes == ({"draft"} if demo.endswith("-draft") else set()), (language, demo, codes)
    finally:
        daemon.cleanup()


def test_signal_sweep_collects_all_comparisons(tmp_path):
    env = demo_environment(tmp_path, "signal-sweep", language="en")
    try:
        client = connect(env)
        run_id = start_run(client, ["start"])
        status = wait_run(client, run_id)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        comparisons = client.call("RunInvocationsRequest", runId=run_id, card=path("score"))
        assert [row.implementationIndex for row in comparisons.records] == list(range(1, 7))
        assert [text(row.implementationLabel) for row in comparisons.records] == [
            f"value={value}, threshold={threshold}"
            for value in (8, 12, 16) for threshold in (10, 14)
        ]
        batch, = comparisons.batches
        assert batch.complete and not batch.failed and batch.invocationCount == 6
        summary = client.call("RunInvocationsRequest", runId=run_id, card=path("summary"))
        assert len(summary.records) == 1
        assert recorded_output(tmp_path / "summary", client, run_id, "summary", port="out") == {"comparisons": 6, "detections": 3}
    finally:
        env.cleanup()


def without_julia_and_r(dst):
    """The Collatz demo of C++ and Python alone, as CI runs it: Julia and R (large Nix closures, a process for every
    step) are left out of the copy, and `compare` loses their two inputs but keeps the rest of its code. The four
    languages together are checked by test_demo_collatz.py (TSUGUMI_TEST_DEMO_RUNS=1)."""
    gone = ("collatz_jl", "collatz_r")
    project = json.loads((dst / "tsugumi.json").read_text())
    canvas = project["canvas"]
    canvas["cards"] = [c for c in canvas["cards"] if c["id"] not in gone]
    canvas["connections"] = [w for w in canvas["connections"] if w["from"]["card"] not in gone and w["to"]["card"] not in gone]
    (dst / "tsugumi.json").write_text(json.dumps(project))
    for name in gone:
        shutil.rmtree(dst / "definitions" / name)
        shutil.rmtree(dst / "externals" / name)
    compare = dst / "definitions" / "compare"
    card = json.loads((compare / "_card.json").read_text())
    card["inputs"] = [i for i in card["inputs"] if i["name"] in ("cpp", "python")]
    (compare / "_card.json").write_text(json.dumps(card))
    code = (compare / "src" / "main.py").read_text()
    code = code.replace('        "Julia": list(inputs.julia.values),\n        "R": list(inputs.r.values),\n', "")
    (compare / "src" / "main.py").write_text(code)


@pytest.fixture
def collatz(tmp_path):
    env = demo_environment(tmp_path, "collatz", edit=without_julia_and_r)
    yield env
    env.cleanup()


@pytest.fixture
def live(tmp_path):
    env = demo_environment(tmp_path, "live-stream")
    yield env
    env.cleanup()


def collatz_reference(n):
    seq = [n]
    while n != 1:
        n = n // 2 if n % 2 == 0 else 3 * n + 1
        seq.append(n)
    return seq


def test_collatz_27(collatz):
    c = connect(collatz)
    rid = start_run(c, ["start"])
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    expected = collatz_reference(27)
    content = artifact_member(c, rid, "record", "collatz.csv").decode()
    rows = list(csv.DictReader(io.StringIO(content)))
    assert [(int(row["k"]), int(row["a_k"]), row["parity"]) for row in rows] == [
        (index, value, "odd" if value % 2 else "even") for index, value in enumerate(expected)
    ]


def test_collatz_rejects_overflow(collatz):
    c = connect(collatz)
    # An odd uint64 whose next Collatz value (3n + 1) cannot fit.
    big = 12297829382473034411
    for card in ("collatz_cpp", "collatz_py"):
        set_typed_param(c, card + "/initial", "value", str(big))
    run_id = start_run(c, ["start"])
    assert wait_run(c, run_id).state == RunState.failed
    triple = c.call("RunRecordRequest", runId=run_id, card=path("collatz_cpp", "triple"))
    assert triple.status.state == CardState.failed
    assert triple.status.failure.kind == FailureKind.output_invalid


def test_report_uses_the_requested_heading(tmp_path):
    env = demo_environment(tmp_path, "report")
    try:
        c = connect(env)
        heading = "Custom heading"
        set_typed_param(c, "render", "heading", heading)
        run_id = start_run(c, ["start"])
        assert wait_run(c, run_id).state == RunState.succeeded
        summary = json.loads(artifact_member(c, run_id, "render", "summary.json"))
        assert summary["heading"] == heading and summary["rows"] > 0

        set_typed_param(c, "aggregate", "fail_on_purpose", True)
        failed = start_run(c, ["start"])
        assert wait_run(c, failed).state == RunState.failed
        aggregate = c.call("RunRecordRequest", runId=failed, card=path("aggregate"))
        assert aggregate.status.state == CardState.failed
        assert not c.call("ArtifactListRequest", runId=failed, card=path("render")).artifacts
    finally:
        env.cleanup()


def chart_session(client):
    return text(client.wait_notification(
        lambda note: (note.valueType == NotificationPayload.ViewOpened
                      and text(note.value.name) == "line-chart"), timeout=180,
    ).value.session)


def chart_points(client, subscription, session, run_id):
    update = wait_for_display(client, run_id, lambda value: text(value.session) == session).value.display
    payload = bytes(update.bytes)
    return_credit(client, subscription, update)
    magic, _, index, count = struct.unpack_from("<IQII", payload)
    assert (magic, index, count) == (0x4D475354, 0, 1)
    payload = payload[20:]
    metadata_size, = struct.unpack_from("<I", payload)
    metadata_end = 4 + metadata_size
    series = json.loads(payload[4:metadata_end])["series"]
    values = next(row for row in series if row["id"] == "value")
    return list(struct.unpack_from(f"<{values['count']}d", payload, metadata_end + values["offset"] * 8))


def send_stream_seed(client, subscription):
    session = text(client.wait_notification(
        lambda note: note.valueType == NotificationPayload.ViewOpened and text(note.value.name) == "seed",
        timeout=90,
    ).value.session)
    ready = client.wait_notification(
        lambda note: note.valueType == NotificationPayload.DisplayUpdated
        and text(note.value.display.session) == session
        and bytes(note.value.display.bytes) == b"\x01",
        timeout=90,
    ).value.display
    client.call("ViewCreditRequest", subscriptionId=subscription, session=session,
                bytes=1, creditId=ready.creditId)
    client.call("ViewSendRequest", session=session, route=ZERO_ROUTE, bytes=b"\x0a")


def test_live_stream_complete_and_ordered(live):
    c = connect(live, client=Client.Web)
    set_typed_param(c, "settings", "interval_ms", 0)
    set_typed_param(c, "settings", "samples", 65)
    subscription = text(c.open().subscriptionId)
    run_id = start_run(c, ["start"], concurrency=2)
    send_stream_seed(c, subscription)
    session = chart_session(c)
    for i in range(65):
        assert chart_points(c, subscription, session, run_id) == list(range(max(0, i - 59), i + 1))
    assert wait_run(c, run_id).state == RunState.succeeded


def test_live_stream_keeps_chart_on_cancel_and_resets_on_rerun(live):
    c = connect(live, client=Client.Web)
    opened = c.open()
    subscription = text(opened.subscriptionId)
    # Keep the first sample and then wait for cancellation, rather than racing
    # the demo's natural completion on a busy runner.
    source = "definitions/generate/src/card.cc"
    snap = c.call("ProjectInspectRequest")
    src = next(s for s in snap.sources if text(s.path) == source)
    code = bytes(c.call("ProjectFileReadRequest", path=source, atRev=snap.rev).bytes).decode()
    delay = "while (std::chrono::steady_clock::now() < until)"
    code = code.replace("  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(s->interval_ms());\n", "")
    code = code.replace(delay, "while (s->index() > 0)")
    c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="test",
           changes=[change("PutSource", path=source, bytes=list(code.encode()), baseHash=text(src.hash))])
    def wait_for_blocked_sample(rid):
        # Display can precede the saved invocation and the next generator call.
        wait_invocation(c, rid, "plot", lambda row: row.state == CardState.succeeded)
        wait_invocation(c, rid, "generate",
                        lambda row: row.state == CardState.running and row.invocation == 2)

    rid = start_run(c, ["start"])
    send_stream_seed(c, subscription)
    wait_for_blocked_sample(rid)
    st = c.call("RunCancelRequest", runId=rid)
    # The reply acknowledges the request before run finalization; it does not
    # join the worker. The run cannot finish normally past its first sample.
    assert st.state == RunState.cancelling
    final = wait_run(c, rid)
    assert final.state == RunState.cancelled
    assert chart_points(c, subscription, chart_session(c), rid) == [0.0]
    record = c.call("RunRecordRequest", runId=rid, card=path("plot"))
    assert record.aggregate.calls == 1
    # A new run starts the reused plotting worker's series from the beginning.
    rid2 = start_run(c, ["start"])
    send_stream_seed(c, subscription)
    assert chart_points(c, subscription, chart_session(c), rid2) == [0.0]
    c.call("RunCancelRequest", runId=rid2)
    assert wait_run(c, rid2).state == RunState.cancelled


@pytest.mark.parametrize("demo,card,members", [
    ("casa", "rgb", {"casa-rgb.png", "provenance.json"}),
    ("gw150914", "chirp", {"chirp.png", "snr.csv"}),
])
@pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_NETWORK_DEMOS") != "1",
                    reason="downloads observations from HEASARC or GWOSC")
def test_network_demo_keeps_results(tmp_path, demo, card, members):
    env = demo_environment(tmp_path, demo, language="en")
    try:
        client = connect(env)
        run_id = start_run(client, "start")
        status = wait_run(client, run_id, timeout=900)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        artifact, = client.call("ArtifactListRequest", runId=run_id, card=path(card)).artifacts
        assert members <= {text(member.path) for member in artifact.members}
    finally:
        env.cleanup()
