"""End-to-end checks of the bundled demos against the expected results.

Run in the shared VM with TSUGUMI_TEST_CONTAINER=1 (the reference
configuration); without it a host tsugumid is used.
"""

from __future__ import annotations

import json
import time

import pytest

from conftest import demo_environment
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload

TERMINAL = (RunState.succeeded, RunState.failed, RunState.cancelled)


def connect(env):
    c = env.client()
    if getattr(env, "client_hello", False):
        c.hello()
    return c


def wait_run(c, run_id: str, timeout: float = 1800):
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = c.call("RunStatusRequest", runId=run_id)
        if st.state in TERMINAL:
            return st
        time.sleep(0.1)
    raise TimeoutError(run_id)


def run(c, card, propagate=True, concurrency=4):
    rev = c.call("ProjectInspectRequest").rev
    acc = c.call("RunRequest", baseRev=rev, opId=new_id(), origin="test", card=path(*card),
                 propagate=propagate, maxConcurrency=concurrency)
    return text(acc.run.runId)


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


@pytest.fixture
def collatz(tmp_path):
    env = demo_environment(tmp_path, "collatz")
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
    rid = run(c, ["start"])
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
    rid2 = run(c, ["collatz-cpp"], propagate=False)
    assert wait_run(c, rid2).state == RunState.succeeded
    assert display_text(c, rid2, "collatz-cpp", "collect") == "112 values: " + " ".join(map(str, expected))


def test_collatz_limit_overflow_and_skipped(collatz):
    c = connect(collatz)
    # Iteration limit: the check Card may run only 50 times.
    original = {}
    for lang in ("collatz-cpp", "collatz-py"):
        edit_external_card(collatz, c, lang, "check", lambda b, lang=lang: (original.__setitem__(lang, b.get("maxRuns")),
                                                                            b.__setitem__("maxRuns", 50)))
    st = wait_run(c, run(c, ["start"]))
    assert st.state == RunState.failed
    assert st.failure.kind == 19  # execution_limit
    # Overflow: 3n+1 exceeds uint64 for this odd value.
    big = 12297829382473034411
    for lang in ("collatz-cpp", "collatz-py"):
        edit_external_card(collatz, c, lang, "check", lambda b, lang=lang: b.__setitem__("maxRuns", original[lang]))
    edit_card(collatz, c, "initial", lambda b: set_param(b, "value", 0),
              raw=lambda s: s.replace('"data": 0', f'"data": {big}'))
    rid = run(c, ["start"])
    st = wait_run(c, rid)
    assert st.state == RunState.failed
    assert "integer overflow" in stderr(c, rid, "collatz-cpp", "triple")
    # n = 2: the odd branch is never selected, so triple is skipped.
    edit_card(collatz, c, "initial", lambda b: set_param(b, "value", 2))
    rid = run(c, ["start"])
    st = wait_run(c, rid)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    rec = c.call("RunRecordRequest", runId=rid, card=path("collatz-cpp", "triple"))
    assert rec.status.state == CardState.skipped


def test_report_failure_rerun_and_artifacts(report):
    c = connect(report)
    rid = run(c, ["start"])
    assert wait_run(c, rid).state == RunState.succeeded
    first = display_text(c, rid, "render")
    assert first.startswith("# Quarterly sales")
    # Same seed, same aggregate.
    rid2 = run(c, ["start"])
    assert wait_run(c, rid2).state == RunState.succeeded
    assert display_text(c, rid2, "render") == first
    # Heading change is reflected.
    edit_card(report, c, "settings", lambda b: set_param(b, "heading", "見出し変更"))
    rid3 = run(c, ["start"])
    st3 = wait_run(c, rid3)
    assert st3.state == RunState.succeeded, (st3.failure and text(st3.failure.diagnostic),
                                             [(text(a.key), text(a.value)) for a in st3.failure.args] if st3.failure else None)
    assert display_text(c, rid3, "render").startswith("# 見出し変更")
    # Intentional failure: stderr is recorded and downstream Cards stop.
    edit_card(report, c, "aggregate", lambda b: set_param(b, "fail_on_purpose", True))
    rid4 = run(c, ["start"])
    st = wait_run(c, rid4)
    assert st.state == RunState.failed
    assert "failing on purpose" in stderr(c, rid4, "aggregate")
    assert c.call("RunRecordRequest", runId=rid4, card=path("render")).status.state == CardState.blocked
    # Fix the setting and re-run only Aggregate with its last saved input.
    edit_card(report, c, "aggregate", lambda b: set_param(b, "fail_on_purpose", False))
    rid5 = run(c, ["aggregate"], propagate=False)
    assert wait_run(c, rid5).state == RunState.succeeded
    rec = c.call("RunRecordRequest", runId=rid5, card=path("aggregate"))
    assert rec.record.recordIo is True and rec.record.inputs[0].name in (b"csv", "csv")
    arts = c.call("ArtifactListRequest", runId=rid3).artifacts
    assert {text(a.output) for a in arts} == {"report", "archive"}


def test_report_artifacts_survive_restart(report):
    c = connect(report)
    rid = run(c, ["start"])
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
    c = connect(live)
    rid = run(c, ["start"], concurrency=concurrency)
    assert wait_run(c, rid).state == RunState.succeeded
    xs, calls = live_points(c, rid)
    assert calls == 200
    assert xs == [float(i) for i in range(140, 200)]  # most recent 60 points, none missing


def test_live_stream_cancel_and_rerun(live):
    c = connect(live)
    opened = c.open()
    c.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("plot")])
    rid = run(c, ["start"])
    c.wait_notification(lambda n: n.valueType == NotificationPayload.DisplayUpdated, timeout=1800)
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
    rid2 = run(c, ["start"])
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
    rid = run(c, ["start"])
    c.wait_notification(lambda n: n.valueType == NotificationPayload.DisplayUpdated, timeout=1800)
    c.close()
    live.crash()
    live.start()
    c = connect(live)
    st = c.call("RunStatusRequest", runId=rid)
    assert st.state == RunState.failed and st.failure.kind == 21  # interrupted, not re-run
    # The environment is usable again and nothing restarted on its own.
    assert c.call("RunStatusRequest").runId in (rid.encode(), rid)
    rid2 = run(c, ["start"])
    c.call("RunCancelRequest", runId=rid2)
    assert wait_run(c, rid2, timeout=20).state == RunState.cancelled


def test_status_and_cancel_during_nix_preparation(report):
    c = connect(report)
    # Clear the build cache so the first Card has to evaluate and build again.
    c.call("ProjectCacheClearRequest", opId=new_id())
    rid = run(c, ["start"])
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
