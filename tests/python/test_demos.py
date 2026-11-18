"""End-to-end checks of the bundled demos against the expected results.

Run in the shared VM with TSUGUMI_TEST_CONTAINER=1 (the reference
configuration); without it a host tsugumid is used.
"""

from __future__ import annotations


import base64
import csv
import io
import json
import re
import shutil

import pytest

from conftest import ROOT, demo_environment
from projects import start_run, wait_for_display as first_display, wait_invocation, wait_run
from tsugumi_client import line_chart_series, change, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.DisplayValue import DisplayValue
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


def edit_external_card(env, c, external: str, card: str, fn):
    """A Card inside a Card in Card: its _card.json is a file of the
    external (this project's copy), changed with PutSource."""
    p = f"externals/{external}/definitions/{card}/_card.json"
    snap = c.call("ProjectInspectRequest")
    src = next(s for s in snap.sources if text(s.path) == p)
    body = json.loads(bytes(c.call("ProjectFileReadRequest", path=p, atRev=snap.rev).bytes))
    fn(body)
    return c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="test",
                  changes=[change("PutSource", path=p, bytes=list(json.dumps(body).encode()), baseHash=text(src.hash))])


def set_param(body: dict, name: str, value):
    next(p for p in body["params"] if p["name"] == name)["data"] = value


def display_text(c, run_id, *card):
    rec = c.call("RunRecordRequest", runId=run_id, card=path(*card))
    return text(rec.record.display.value.text) if rec.record and rec.record.display else None


def stderr(c, run_id, *card):
    return text(c.call("LogRangeRequest", runId=run_id, card=path(*card), stream=2, limitBytes=262144).text)


DEMOS = json.loads((ROOT / "demos" / "ja" / "index.json").read_text())["demos"]


@pytest.mark.parametrize("language", ["ja", "en"])
@pytest.mark.parametrize("demo", [d["path"] for d in DEMOS])
def test_every_listed_demo_resolves(tmp_path, demo, language):
    """Each demo on the list, in either language, opens and resolves:
    implemented ones without a diagnostic, a Draft-only plan with nothing but
    its Drafts."""
    env = demo_environment(tmp_path, demo, language=language)
    try:
        c = connect(env)
        snap = c.call("ProjectInspectRequest")
        codes = {(text(r.card.segments[-1]), text(d.code)) for r in snap.resolution for d in r.diagnostics}
        if demo.endswith("-draft"):
            assert codes and {code for _, code in codes} == {"draft"}, codes
        else:
            assert not codes, codes
    finally:
        env.cleanup()


@pytest.mark.parametrize(("language", "expected"), [
    ("en", "3 of 6 comparisons passed across 2 thresholds."),
    ("ja", "2 個のしきい値で比較し、6 件のうち 3 件を検出しました。"),
])
def test_signal_sweep_collects_all_comparisons(tmp_path, language, expected):
    """Run both versions through real Nix evaluation and the ordinary Card worker."""
    env = demo_environment(tmp_path, "signal-sweep", language=language)
    try:
        client = connect(env)
        run_id = start_run(client, ["start"])
        status = wait_run(client, run_id)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        comparisons = client.call("RunInvocationsRequest", runId=run_id, card=path("score"))
        assert [row.implementationIndex for row in comparisons.records] == [1, 2] * 3
        batch, = comparisons.batches
        assert batch.complete and not batch.failed and batch.invocationCount == 6
        summary = client.call("RunInvocationsRequest", runId=run_id, card=path("summary"))
        assert len(summary.records) == 1
        assert display_text(client, run_id, "summary") == expected
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
    both = dst / "definitions" / "both" / "_card.json"
    fork = json.loads(both.read_text())
    set_param(fork, "branches", ["cpp", "python"])
    both.write_text(json.dumps(fork))
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
def report(tmp_path):
    env = demo_environment(tmp_path, "report")
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


def check_collatz_summary(shown, sequence):
    assert shown.markdown
    values = set(map(int, re.findall(r"\d+", text(shown.text))))
    assert {len(sequence), len(sequence) - 1, sequence[0], max(sequence)} <= values


def test_collatz_27(collatz):
    c = connect(collatz)
    rid = start_run(c, ["start"])
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    expected = collatz_reference(27)
    assert len(expected) == 112
    shown = c.call("RunRecordRequest", runId=rid, card=path("compare")).record.display.value
    check_collatz_summary(shown, expected)
    # The agreed sequence is kept as a CSV file (a File artifact of record).
    art = next(a for a in c.call("ArtifactListRequest", runId=rid).artifacts if text(a.output) == "table")
    content = bytes(c.call("ArtifactReadRequest", artifactId=text(art.artifactId)).bytes).decode()
    rows = list(csv.DictReader(io.StringIO(content)))
    assert [(int(row["k"]), int(row["a_k"]), row["parity"]) for row in rows] == [
        (index, value, "odd" if value % 2 else "even") for index, value in enumerate(expected)
    ]
    for lang in ("collatz-cpp", "collatz-py"):
        assert collected_collatz(c, rid, lang, "collect") == expected
    # The C++ Card in Card runs alone from its boundary input (saved value).
    rid2 = start_run(c, ["collatz-cpp"], propagate=False)
    assert wait_run(c, rid2).state == RunState.succeeded
    assert display_text(c, rid2, "collatz-cpp", "collect") == "112 values: " + " ".join(map(str, expected))


def test_collatz_limit_overflow_and_skipped(collatz):
    c = connect(collatz)
    # Iteration limit: the check Card may run only 50 times.
    original = {}
    for lang in ("collatz-cpp", "collatz-py"):
        edit_external_card(collatz, c, lang, "check", lambda b, lang=lang: (original.__setitem__(lang, b.get("maxRuns")),
                                                                            b.__setitem__("maxRuns", 50)))
    st = wait_run(c, start_run(c, ["start"]))
    assert st.state == RunState.failed
    assert st.failure.kind == 19  # execution_limit
    # Overflow: 3n+1 exceeds uint64 for this odd value.
    big = 12297829382473034411
    for lang in ("collatz-cpp", "collatz-py"):
        edit_external_card(collatz, c, lang, "check", lambda b, lang=lang: b.__setitem__("maxRuns", original[lang]))
    edit_card(collatz, c, "initial", lambda b: set_param(b, "value", 0),
              raw=lambda s: s.replace('"data": 0', f'"data": {big}'))
    rid = start_run(c, ["start"])
    st = wait_run(c, rid)
    assert st.state == RunState.failed
    assert "integer overflow" in stderr(c, rid, "collatz-cpp", "triple")
    # n = 2: the odd branch is never selected, so triple is skipped.
    edit_card(collatz, c, "initial", lambda b: set_param(b, "value", 2))
    rid = start_run(c, ["start"])
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    rec = c.call("RunRecordRequest", runId=rid, card=path("collatz-cpp", "triple"))
    assert rec.status.state == CardState.skipped


def test_report_failure_rerun_and_artifacts(report):
    c = connect(report)
    rid = start_run(c, ["start"])
    assert wait_run(c, rid).state == RunState.succeeded
    first = display_text(c, rid, "render")
    assert first.startswith("# Quarterly sales")
    # Same seed, same aggregate.
    rid2 = start_run(c, ["start"])
    assert wait_run(c, rid2).state == RunState.succeeded
    assert display_text(c, rid2, "render") == first
    # Heading change is reflected.
    edit_card(report, c, "settings", lambda b: set_param(b, "heading", "見出し変更"))
    rid3 = start_run(c, ["start"])
    st3 = wait_run(c, rid3)
    assert st3.state == RunState.succeeded, (st3.failure and text(st3.failure.diagnostic),
                                             [(text(a.key), text(a.value)) for a in st3.failure.args] if st3.failure else None)
    assert display_text(c, rid3, "render").startswith("# 見出し変更")
    # Intentional failure: stderr is recorded and downstream Cards stop.
    edit_card(report, c, "aggregate", lambda b: set_param(b, "fail_on_purpose", True))
    rid4 = start_run(c, ["start"])
    st = wait_run(c, rid4)
    assert st.state == RunState.failed
    assert "failing on purpose" in stderr(c, rid4, "aggregate")
    assert c.call("RunRecordRequest", runId=rid4, card=path("render")).status.state == CardState.blocked
    # Fix the setting and re-run only Aggregate with its last saved input.
    edit_card(report, c, "aggregate", lambda b: set_param(b, "fail_on_purpose", False))
    rid5 = start_run(c, ["aggregate"], propagate=False)
    assert wait_run(c, rid5).state == RunState.succeeded
    rec = c.call("RunRecordRequest", runId=rid5, card=path("aggregate"))
    assert rec.record.recordIo is True and rec.record.inputs[0].name in (b"csv", "csv")
    arts = c.call("ArtifactListRequest", runId=rid3).artifacts
    assert {text(a.output) for a in arts} == {"report", "archive"}


def test_report_artifacts_survive_restart(report):
    c = connect(report)
    rid = start_run(c, ["start"])
    assert wait_run(c, rid).state == RunState.succeeded
    art = next(a for a in c.call("ArtifactListRequest", runId=rid).artifacts if text(a.output) == "report")
    c.close()
    report.stop()
    report.start()
    c = connect(report)
    content = c.call("ArtifactReadRequest", artifactId=text(art.artifactId), member="report.md")
    assert bytes(content.bytes).decode().startswith("# Quarterly sales")


def live_points(c, rid):
    rec = c.call("RunRecordRequest", runId=rid, card=path("plot"))
    if rec.record.display is None:
        # A cancel that lands in an invocation before it has shown anything ends
        # that invocation without a display; the last chart is the one before.
        rec = c.call("RunRecordRequest", runId=rid, card=path("plot"), invocation=rec.record.summary.invocation - 1)
    series = line_chart_series(rec.record.display.value)
    return [float(x) for x in series[0].x], rec.aggregate.calls


@pytest.mark.parametrize("concurrency", [1, 2])
def test_live_stream_complete_and_ordered(live, concurrency):
    # Count and order only: no interval between samples (the pace is its own test).
    c = connect(live)
    edit_card(live, c, "settings", lambda b: set_param(b, "interval_ms", 0))
    rid = start_run(c, ["start"], concurrency=concurrency)
    assert wait_run(c, rid).state == RunState.succeeded
    xs, calls = live_points(c, rid)
    assert calls == 200
    assert xs == [float(i) for i in range(140, 200)]  # most recent 60 points, none missing


def test_live_stream_keeps_its_pace(live):
    # The demo should retain all five samples with its configured interval.
    interval_ms = 100
    c = connect(live)
    def configure(body):
        set_param(body, "samples", 5)
        set_param(body, "interval_ms", interval_ms)
    edit_card(live, c, "settings", configure)
    rid = start_run(c, ["start"])
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded
    xs, calls = live_points(c, rid)
    assert calls == 5 and xs == [0.0, 1.0, 2.0, 3.0, 4.0]
    # Measured by the core from start to end: at least the four waits between five samples.
    assert st.endedAt - st.startedAt >= (calls - 1) * interval_ms, st.endedAt - st.startedAt


def test_live_stream_cancel_and_rerun(live):
    c = connect(live)
    opened = c.open()
    c.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("plot")])
    # Keep the first sample and then wait for cancellation, rather than racing
    # the demo's 20-second natural completion on a busy runner. The separate
    # pace test exercises the unmodified sampling delay.
    source = "definitions/generate/src/card.cc"
    snap = c.call("ProjectInspectRequest")
    src = next(s for s in snap.sources if text(s.path) == source)
    code = bytes(c.call("ProjectFileReadRequest", path=source, atRev=snap.rev).bytes).decode()
    delay = "while (std::chrono::steady_clock::now() < until)"
    assert delay in code
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
    first_display(c, rid)
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
    wait_for_blocked_sample(rid2)
    assert c.call("RunCancelRequest", runId=rid2).state == RunState.cancelling
    assert wait_run(c, rid2).state == RunState.cancelled


def test_crash_marks_run_interrupted_and_recovers(live):
    c = connect(live)
    opened = c.open()
    c.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("plot")])
    rid = start_run(c, ["start"])
    first_display(c, rid)
    c.close()
    live.crash()
    live.start()
    c = connect(live)
    st = c.call("RunStatusRequest", runId=rid)
    assert st.state == RunState.failed and st.failure.kind == 21  # interrupted, not re-run
    # The environment is usable again and nothing restarted on its own.
    assert c.call("RunStatusRequest").runId in (rid.encode(), rid)
    rid2 = start_run(c, ["start"])
    c.call("RunCancelRequest", runId=rid2)
    assert wait_run(c, rid2).state == RunState.cancelled




@pytest.fixture
def plots_demo(tmp_path):
    env = demo_environment(tmp_path, "plots")
    yield env
    env.cleanup()


def test_plots_draw_into_the_displays(plots_demo):
    """Plain matplotlib (plt.show(), a FuncAnimation) reaches the Cards'
    displays through the SDK's backend: the animation live, frame by frame,
    and each Card's last image sharp (2x) and named after its title; the
    Japanese in a title draws with the Card's fallback font. Plotly's
    fig.show() and Altair's chart.show() send interactive plots (module
    displays) with the figure's JSON and the chart's inline data."""
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
    assert "missing from font" not in stderr(c, rid, "summary")
    from PIL import Image

    for card in ("summary", "diffusion"):
        display = c.call("RunRecordRequest", runId=rid, card=path(card)).record.display
        assert display.valueType == DisplayValue.DisplayImage, card
        image = display.value
        assert text(image.mediaType) == "image/png" and text(image.alt)
        if card == "diffusion":
            assert re.findall(r"\d+", text(image.alt))[-2:] == ["80", "80"]
        with Image.open(io.BytesIO(bytes(image.data))) as rendered:
            assert rendered.format == "PNG"
            rendered.load()
            assert rendered.width >= 1200
    import numpy as np

    def coordinates(values):
        if isinstance(values, list):
            return values
        return np.frombuffer(base64.b64decode(values["bdata"]), dtype=values["dtype"])

    explore = c.call("RunRecordRequest", runId=rid, card=path("explore")).record.display
    assert explore.valueType == DisplayValue.DisplayModule
    figure = json.loads(text(explore.value.configJson))
    walkers = [(end, reach) for trace in figure["data"] if "x" in trace and "y" in trace
               for end, reach in zip(coordinates(trace["x"]), coordinates(trace["y"]), strict=True)]
    assert len(walkers) == 200 and all(abs(end) <= reach for end, reach in walkers)
    spread = c.call("RunRecordRequest", runId=rid, card=path("spread")).record.display
    assert spread.valueType == DisplayValue.DisplayModule
    spec = json.loads(text(spread.value.configJson))
    rows = [row for values in spec.get("datasets", {}).values() for row in values]
    rows += spec.get("data", {}).get("values", [])
    by_series = {}
    for row in rows:
        by_series.setdefault(row["series"], {})[row["step"]] = row["value"]
    assert len(by_series) == 2
    measured, = [series for series in by_series.values() if any(step != value for step, value in series.items())]
    reference, = [series for series in by_series.values() if all(step == value for step, value in series.items())]
    assert measured.keys() == reference.keys()
    assert measured[0] == 0 and measured[80] > 0
