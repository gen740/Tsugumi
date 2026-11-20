"""End-to-end checks of the bundled demos against the expected results.

Run in the shared VM with TSUGUMI_TEST_CONTAINER=1 (the reference
configuration); without it a host tsugumid is used.
"""

from __future__ import annotations


import base64
import csv
import io
import json
import shutil

import pytest

from conftest import ROOT, Daemon, demo_environment, write_project
from harness.observations import read_pages
from harness.values import reference_json
from projects import start_run, wait_for_display as first_display, wait_invocation, wait_run
from tsugumi_client import line_chart_series, change, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.RunEventValue import RunEventValue

def connect(env):
    c = env.client()
    if getattr(env, "client_hello", False):
        c.hello()
    return c


def edit_card(env, c, card: str, fn, raw=lambda s: s):
    """Changes a Card's settings (its _card.json) through a save; `raw`
    rewrites the JSON text after `fn` (spellings JSON numbers cannot keep)."""
    snap = c.call("ProjectInspectRequest")
    d = next(x for x in snap.definitions if text(x.id) == card)
    body = json.loads(text(d.cardJson))
    fn(body)
    meta = {"metaJson": text(d.metaJson)} if d.metaJson else {}
    return c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="test",
                  changes=[change("PutDefinition", id=card, cardJson=raw(json.dumps(body)), **meta)])


def set_param(body: dict, name: str, value):
    next(p for p in body["params"] if p["name"] == name)["data"] = value


def display_text(c, run_id, *card):
    rec = c.call("RunRecordRequest", runId=run_id, card=path(*card))
    return text(rec.record.display.value.text) if rec.record and rec.record.display else None


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
                                ignore=shutil.ignore_patterns(".tsugumi-history.json"))
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
        assert sorted(row.implementationIndex for row in comparisons.records) == [1] * 3 + [2] * 3
        batch, = comparisons.batches
        assert batch.complete and not batch.failed and batch.invocationCount == 6
        summary = client.call("RunInvocationsRequest", runId=run_id, card=path("summary"))
        assert len(summary.records) == 1
        record = client.call("RunRecordRequest", runId=run_id, card=path("summary"))
        output, = record.record.outputs
        assert reference_json(tmp_path / "summary", bytes(output.schemaBfbs), bytes(output.inlineBytes),
                              root_type=text(output.rootType)) == {"comparisons": 6, "detections": 3}
    finally:
        env.cleanup()


def without_julia_and_r(dst):
    """The Collatz demo of C++ and Python alone, as CI runs it: Julia and R (large Nix closures, a process for every
    step) are left out of the copy, and `compare` loses their two inputs but keeps the rest of its code. The four
    languages together are checked by test_demo_collatz.py (TSUGUMI_TEST_DEMO_RUNS=1)."""
    gone = ("collatz-jl", "collatz-r")
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
    for old, new in [('        "Julia": list(inputs.julia.values),\n        "R": list(inputs.r.values),\n', ""),
                     ("C++, Python, Julia and R agree", "C++ and Python agree")]:
        assert old in code, old
        code = code.replace(old, new)
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


def collected_collatz(client, run_id, *card):
    display = display_text(client, run_id, *card)
    _, _, sequence = display.partition(":")
    return [int(value) for value in sequence.split()]


def test_collatz_27(collatz):
    c = connect(collatz)
    rid = start_run(c, ["start"])
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    expected = collatz_reference(27)
    # The agreed sequence is kept as a CSV file (a File artifact of record).
    art = next(a for a in c.call("ArtifactListRequest", runId=rid).artifacts if text(a.output) == "table")
    content = bytes(c.call("ArtifactReadRequest", artifactId=text(art.artifactId)).bytes).decode()
    rows = list(csv.DictReader(io.StringIO(content)))
    assert [(int(row["k"]), int(row["a_k"]), row["parity"]) for row in rows] == [
        (index, value, "odd" if value % 2 else "even") for index, value in enumerate(expected)
    ]


def test_collatz_rejects_overflow(collatz):
    c = connect(collatz)
    # An odd uint64 whose next Collatz value (3n + 1) cannot fit.
    big = 12297829382473034411
    edit_card(collatz, c, "initial", lambda b: set_param(b, "value", 0),
              raw=lambda s: s.replace('"data": 0', f'"data": {big}'))
    run_id = start_run(c, ["start"])
    assert wait_run(c, run_id).state == RunState.failed
    triple = c.call("RunRecordRequest", runId=run_id, card=path("collatz-cpp", "triple"))
    assert triple.status.state == CardState.failed
    assert triple.status.failure.kind == FailureKind.output_invalid


def test_report_uses_the_requested_heading(tmp_path):
    env = demo_environment(tmp_path, "report")
    try:
        c = connect(env)
        heading = "Custom heading"
        edit_card(env, c, "settings", lambda b: set_param(b, "heading", heading))
        run_id = start_run(c, ["start"])
        assert wait_run(c, run_id).state == RunState.succeeded
        report = next(a for a in c.call("ArtifactListRequest", runId=run_id).artifacts if text(a.output) == "report")
        summary = json.loads(read_pages(c, "ArtifactReadRequest", artifactId=text(report.artifactId),
                                        member="summary.json", page_bytes=65536))
        assert summary["heading"] == heading and summary["rows"] > 0

        edit_card(env, c, "aggregate", lambda b: set_param(b, "fail_on_purpose", True))
        failed = start_run(c, ["start"])
        assert wait_run(c, failed).state == RunState.failed
        aggregate = c.call("RunRecordRequest", runId=failed, card=path("aggregate"))
        assert aggregate.status.state == CardState.failed
        assert not c.call("ArtifactListRequest", runId=failed, card=path("render")).artifacts
    finally:
        env.cleanup()


def live_points(c, rid):
    rec = c.call("RunRecordRequest", runId=rid, card=path("plot"))
    if rec.record.display is None:
        # A cancel that lands in an invocation before it has shown anything ends
        # that invocation without a display; the last chart is the one before.
        rec = c.call("RunRecordRequest", runId=rid, card=path("plot"), invocation=rec.record.summary.invocation - 1)
    series = line_chart_series(rec.record.display.value)
    return [float(x) for x in series[0].x], rec.aggregate.calls


def test_live_stream_complete_and_ordered(live):
    c = connect(live)
    def settings(body):
        set_param(body, "interval_ms", 0)
        set_param(body, "samples", 65)

    edit_card(live, c, "settings", settings)
    run_id = start_run(c, ["start"], concurrency=2)
    assert wait_run(c, run_id).state == RunState.succeeded
    xs, _ = live_points(c, run_id)
    assert xs == list(range(5, 65))


def test_live_stream_keeps_chart_on_cancel_and_resets_on_rerun(live):
    c = connect(live)
    opened = c.open()
    c.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("plot")])
    # Keep the first sample and then wait for cancellation, rather than racing
    # the demo's natural completion on a busy runner.
    source = "definitions/generate/src/card.cc"
    snap = c.call("ProjectInspectRequest")
    src = next(s for s in snap.sources if text(s.path) == source)
    code = bytes(c.call("ProjectFileReadRequest", path=source, atRev=snap.rev).bytes).decode()
    delay = "while (std::chrono::steady_clock::now() < until)"
    code = code.replace("  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(s.interval_ms);\n", "")
    code = code.replace(delay, "while (s.index > 0)")
    c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="test",
           changes=[change("PutSource", path=source, bytes=list(code.encode()), baseHash=text(src.hash))])
    def wait_for_blocked_sample(rid):
        # Display can precede the saved invocation and the next generator call.
        wait_invocation(c, rid, "plot", lambda row: row.state == CardState.succeeded)
        wait_invocation(c, rid, "generate",
                        lambda row: row.state == CardState.running and row.invocation == 2)

    rid = start_run(c, ["start"])
    wait_for_blocked_sample(rid)
    st = c.call("RunCancelRequest", runId=rid)
    # The reply acknowledges the request before run finalization; it does not
    # join the worker. The run cannot finish normally past its first sample.
    assert st.state == RunState.cancelling
    final = wait_run(c, rid)
    assert final.state == RunState.cancelled
    xs, calls = live_points(c, rid)
    assert calls == 1 and xs == [0.0]  # the last chart is kept
    # A new run starts the reused plotting worker's series from the beginning.
    rid2 = start_run(c, ["start"])
    first = first_display(c, rid2)
    assert line_chart_series(first.value.display.value)[0].x == (0.0,)
    c.call("RunCancelRequest", runId=rid2)
    assert wait_run(c, rid2).state == RunState.cancelled


@pytest.fixture
def plots_demo(tmp_path):
    env = demo_environment(tmp_path, "plots")
    yield env
    env.cleanup()


def test_plots_draw_into_the_displays(plots_demo):
    """Plain matplotlib (plt.show(), a FuncAnimation) reaches the Cards'
    displays through the SDK backend while the run is active. Plotly and
    Altair send the demo's computed data in their interactive displays."""
    c = connect(plots_demo)
    edit_card(plots_demo, c, "settings", lambda b: (set_param(b, "walkers", 200), set_param(b, "steps", 80)))
    edit_card(plots_demo, c, "diffusion", lambda b: set_param(b, "frames", 12))
    opened = c.open()
    c.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path(c) for c in ("summary", "diffusion", "explore", "spread")])
    rid = start_run(c, ["start"])
    frames = []
    while True:
        note = c.wait_notification(lambda n: (
            n.valueType == NotificationPayload.DisplayUpdated and text(n.value.display.runId) == rid
        ) or (
            n.valueType == NotificationPayload.RunEvent and text(n.value.runId) == rid
            and n.value.valueType == RunEventValue.RunFinished
        ))
        if note.valueType == NotificationPayload.RunEvent:
            break
        frames.append(text(note.value.display.card.segments[-1]))
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded, (stderr(c, rid, "summary"), stderr(c, rid, "diffusion"))
    assert "diffusion" in frames  # A display was received before RunFinished.
    import numpy as np

    def coordinates(values):
        if isinstance(values, list):
            return values
        return np.frombuffer(base64.b64decode(values["bdata"]), dtype=values["dtype"])

    explore = c.call("RunRecordRequest", runId=rid, card=path("explore")).record.display
    figure = json.loads(text(explore.value.configJson))
    walkers = [(end, reach) for trace in figure["data"] if "x" in trace and "y" in trace
               for end, reach in zip(coordinates(trace["x"]), coordinates(trace["y"]), strict=True)]
    assert len(walkers) == 200 and all(abs(end) <= reach for end, reach in walkers)
    spread = c.call("RunRecordRequest", runId=rid, card=path("spread")).record.display
    spec = json.loads(text(spread.value.configJson))
    rows = [row for values in spec.get("datasets", {}).values() for row in values]
    rows += spec.get("data", {}).get("values", [])
    by_series = {}
    for row in rows:
        by_series.setdefault(row["series"], {})[row["step"]] = row["value"]
    measured, = [series for series in by_series.values() if any(step != value for step, value in series.items())]
    reference, = [series for series in by_series.values() if all(step == value for step, value in series.items())]
    assert measured.keys() == reference.keys()
    assert measured[0] == 0 and measured[80] > 0
