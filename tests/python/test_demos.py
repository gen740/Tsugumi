"""End-to-end checks of the bundled demos against the expected results.

Run in the shared VM with TSUGUMI_TEST_CONTAINER=1 (the reference
configuration); without it a host tsugumid is used.
"""

from __future__ import annotations

import json
import shutil
import time

import pytest

from conftest import ROOT, demo_environment
from projects import ENDED, start_run, wait_run
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload

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


DEMOS = json.loads((ROOT / "demos" / "index.json").read_text())["demos"]


def test_every_implemented_demo_has_a_field():
    """The guide's first questions list the demos by field (demos/index.json
    "field"): each implemented demo is in one; a Draft is listed apart."""
    bad = [d["id"] for d in DEMOS if bool(d.get("field", "").strip()) == d["id"].endswith("-draft")]
    assert not bad, bad


def test_every_bundled_card_says_what_it_does():
    """Every definition of every demo, inside Cards in Card too, has a summary
    in its _meta.json, which is where the Canvas and Card settings read it."""
    definitions = sorted(p for p in (ROOT / "demos").rglob("definitions/*") if p.is_dir())
    assert len(definitions) > 200
    bad = []
    for d in definitions:
        where = d.relative_to(ROOT / "demos")
        meta = d / "_meta.json"
        summary = json.loads(meta.read_text()).get("description", "").strip() if meta.exists() else ""
        if not summary or summary == d.name:
            bad.append(f"{where}: no summary")
        card = d / "_card.json"
        if card.exists() and "description" in json.loads(card.read_text()):
            bad.append(f"{where}: description in _card.json")
    assert not bad, bad


@pytest.mark.parametrize("demo", [d["path"] for d in DEMOS])
def test_every_listed_demo_resolves(tmp_path, demo):
    """Each demo on the list opens and resolves: implemented ones without a
    diagnostic, a Draft-only plan with nothing but its Drafts."""
    env = demo_environment(tmp_path, demo)
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
    for old, new in [("from inputs.julia.collatz.Sequence import Sequence as JuliaSequence\n", ""),
                     ("from inputs.r.collatz.Sequence import Sequence as RSequence\n", ""),
                     ('        "Julia": values(ctx.input("julia", JuliaSequence)),\n        "R": values(ctx.input("r", RSequence)),\n', ""),
                     ("C++, Python, Julia and R agree", "C++ and Python agree")]:
        assert old in code, old
        code = code.replace(old, new)
    (compare / "src" / "main.py").write_text(code)


def test_the_mnist_draft_is_work_for_helpers_that_meets_again():
    """Seven Drafts helpers can write at the same time (no wire between them,
    no more than delegate takes), with nothing of an answer in them, and one
    output type written out in each so that compare can read what they wrote."""
    demo = ROOT / "demos" / "mnist-classifiers-draft"
    doc = json.loads((demo / "tsugumi.json").read_text())
    classifiers = {"knn", "logistic", "svm", "forest", "boosting", "mlp", "cnn"}
    template = {c["id"]: json.loads((demo / "definitions" / c["id"] / "_card.json").read_text()).get("template")
                for c in doc["canvas"]["cards"]}
    assert {card for card, t in template.items() if t == "draft"} == classifiers | {"compare"}
    assert (demo / "definitions" / "load" / "src" / "main.py").is_file()
    assert (demo / "definitions" / "prepare" / "src" / "main.py").is_file()
    wires = {(w["from"]["card"], w["to"]["card"]) for w in doc["canvas"]["connections"]}
    assert not {w for w in wires if w[0] in classifiers and w[1] in classifiers}
    assert {("share", m) for m in classifiers} | {(m, "compare") for m in classifiers} <= wires
    limit = json.loads((ROOT / "agent" / "Shared" / "agent.json").read_text())["delegateTool"]
    assert len(classifiers) <= limit["inputSchema"]["properties"]["tasks"]["maxItems"]
    # The type of the finished demo, the one compare reads.
    finished = (ROOT / "demos" / "mnist-classifiers" / "definitions" / "knn" / "_outputs.fbs").read_text()
    contract = " ".join(finished.split())
    for card in classifiers | {"compare"}:
        folder = demo / "definitions" / card
        assert sorted(f.name for f in folder.iterdir()) == ["_card.json", "_meta.json"], card
        content = json.loads((folder / "_meta.json").read_text())["content"]
        assert "assert" in content, card
        if card != "compare":
            assert contract in " ".join(content.split()), card


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


def test_collatz_27(collatz):
    c = connect(collatz)
    rid = start_run(c, ["start"])
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    expected = collatz_reference(27)
    assert len(expected) == 112
    shown = c.call("RunRecordRequest", runId=rid, card=path("compare")).record.display.value
    assert shown.markdown and text(shown.text).startswith("**C++ and Python agree**: 112 values, $a_0 = 27$")
    assert "after $111$ steps" in text(shown.text) and "9232" in text(shown.text)
    # The agreed sequence is kept as a CSV file (a File artifact of record).
    art = next(a for a in c.call("ArtifactListRequest", runId=rid).artifacts if text(a.output) == "table")
    csv = bytes(c.call("ArtifactReadRequest", artifactId=text(art.artifactId)).bytes).decode()
    lines = csv.splitlines()
    assert lines[0] == "k,a_k,parity" and lines[1] == "0,27,odd" and lines[-1] == "111,1,odd" and len(lines) == 113
    for lang in ("collatz-cpp", "collatz-py"):
        shown = display_text(c, rid, lang, "collect")
        assert shown == "112 values: " + " ".join(map(str, expected))
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
    series = rec.record.display.value.series
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
    # A few samples at the demo's 100 ms: the loop waits between them.
    c = connect(live)
    edit_card(live, c, "settings", lambda b: set_param(b, "samples", 5))
    rid = start_run(c, ["start"])
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded
    xs, calls = live_points(c, rid)
    assert calls == 5 and xs == [0.0, 1.0, 2.0, 3.0, 4.0]
    # Measured by the core from start to end: at least the four waits between five samples.
    assert st.endedAt - st.startedAt >= 400, st.endedAt - st.startedAt


def first_display(c, rid: str, timeout: float = 1800):
    """The run's first Display; a run that ends without one fails the test
    at once (a build that could not download, say), never after the timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            return c.wait_notification(lambda n: n.valueType == NotificationPayload.DisplayUpdated, timeout=5)
        except TimeoutError:
            st = c.call("RunStatusRequest", runId=rid)
            assert st.state not in ENDED, f"the run ended ({st.state}) before showing anything: {st.failure}"
    raise TimeoutError("no Display")


def test_live_stream_cancel_and_rerun(live):
    c = connect(live)
    opened = c.open()
    c.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("plot")])
    rid = start_run(c, ["start"])
    first_display(c, rid)
    time.sleep(1.0)
    t0 = time.time()
    st = c.call("RunCancelRequest", runId=rid)
    assert time.time() - t0 < 0.1
    assert st.state in (RunState.cancelling, RunState.cancelled)
    final = wait_run(c, rid, timeout=10)
    assert final.state == RunState.cancelled and time.time() - t0 < 2.0
    xs, calls = live_points(c, rid)
    assert 0 < calls < 200 and xs  # the last chart is kept
    # A new run starts the series from the beginning.
    rid2 = start_run(c, ["start"])
    c.wait_notification(lambda n: n.valueType == NotificationPayload.DisplayUpdated and
                        text(n.value.display.runId) == rid2, timeout=60)
    first = c.wait_notification(lambda n: n.valueType == NotificationPayload.DisplayUpdated and
                                text(n.value.display.runId) == rid2, timeout=60)
    assert first.value.display.value.series[0].x[0] == 0.0
    c.call("RunCancelRequest", runId=rid2)
    wait_run(c, rid2, timeout=10)


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
    assert wait_run(c, rid2, timeout=20).state == RunState.cancelled


def test_status_and_cancel_during_nix_preparation(report):
    c = connect(report)
    # Clear the build cache so the first Card has to evaluate and build again.
    c.call("ProjectCacheClearRequest", opId=new_id())
    rid = start_run(c, ["start"])
    other = connect(report)
    seen_preparing = False
    for _ in range(600):
        t0 = time.time()
        st = other.call("RunStatusRequest", runId=rid)
        assert time.time() - t0 < 0.5  # status never waits for the Nix lane
        if st.state == RunState.preparing:
            seen_preparing = True
            break
        if st.state in TERMINAL:
            break
        time.sleep(0.05)
    t0 = time.time()
    other.call("RunCancelRequest", runId=rid)
    final = wait_run(other, rid, timeout=60)
    print(f"cancel during preparation took {time.time() - t0:.2f}s (seen preparing: {seen_preparing})")
    assert final.state in (RunState.cancelled, RunState.succeeded)


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
    first_display(c, rid)
    while True:
        try:
            n = c.wait_notification(lambda n: n.valueType == NotificationPayload.DisplayUpdated and
                                    text(n.value.display.runId) == rid, timeout=5)
            frames.append(text(n.value.display.card.segments[-1]))
        except TimeoutError:
            if c.call("RunStatusRequest", runId=rid).state in ENDED:
                break
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded, (stderr(c, rid, "summary"), stderr(c, rid, "diffusion"))
    assert frames.count("diffusion") >= 3, frames  # the animation was seen while it played
    assert "missing from font" not in stderr(c, rid, "summary")
    for card, alt in (("summary", "Random walks spread as √t（拡散は √t で広がる）"), ("diffusion", "step 80 of 80")):
        d = c.call("RunRecordRequest", runId=rid, card=path(card)).record.display
        assert d.valueType == 5, card  # DisplayImage
        img = d.value
        assert text(img.mediaType) == "image/png" and text(img.alt) == alt
        data = bytes(img.data)
        assert data[:8] == b"\x89PNG\r\n\x1a\n"
        width = int.from_bytes(data[16:20], "big")
        assert width >= 1200, width  # 2x of a 9-inch figure at 100 dpi (cropped to its content for summary)
    d = c.call("RunRecordRequest", runId=rid, card=path("explore")).record.display
    assert d.valueType == 7 and text(d.value.name) == "plotly"  # DisplayModule
    fig = json.loads(text(d.value.dataJson))
    assert fig["data"][0]["type"] == "scatter" and "200 walkers" in fig["layout"]["title"]["text"]
    d = c.call("RunRecordRequest", runId=rid, card=path("spread")).record.display
    assert d.valueType == 7 and text(d.value.name) == "vega-lite"
    spec = json.loads(text(d.value.dataJson))
    assert spec["$schema"].startswith("https://vega.github.io/schema/vega-lite/v6")
    assert len(spec["layer"]) == 2
    assert spec.get("datasets") or spec.get("data", {}).get("values")  # the data is in the spec
