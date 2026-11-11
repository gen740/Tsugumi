"""Scheduler behaviour with builtin Cards only (no Nix): consumption order of
all/any inputs, non-firing optional outputs, maxRuns, no lost or duplicated
tokens, subgraph runs through their boundary, and per-path history."""

from __future__ import annotations

import json
from contextlib import ExitStack
from pathlib import Path

import pytest

from harness.daemon import Daemon
from projects import card, card_in_card, conn, run_and_wait, write
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.Phase import Phase
from Tsugumi.Data.StageOutcome import StageOutcome
from Tsugumi.Data.RunState import RunState

MSG_FBS = """
namespace demo;
table Msg { text:string; }
table Outputs { msg:Msg (required); }
root_type Outputs;
"""

FLAG_FBS = """
namespace demo;
table Flag { value:bool; }
table Outputs { flag:Flag (required); }
root_type Outputs;
"""

BASE_DEFS = {
    "trigger": {"template": "trigger"},
    "msg": {"template": "input", "params": [{"name": "text", "type": "string", "data": "hello"}], "_outputs": MSG_FBS},
    "flag": {"template": "input", "params": [{"name": "value", "type": "bool", "data": True}], "_outputs": FLAG_FBS},
    "fork": {"template": "fork"},
    "branch": {"template": "branch"},
    "merge": {"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}], "maxRuns": 4},
    "log": {"template": "logger", "maxRuns": 4},
}


def logs(c, run_id, card_id):
    page = c.call("RunInvocationsRequest", runId=run_id, card=path(*card_id.split("/")))
    out = []
    for r in page.records:
        if r.state != CardState.succeeded:
            continue
        s = c.call("LogRangeRequest", runId=run_id, card=path(*card_id.split("/")), invocation=r.invocation, stream=1)
        out.append(text(s.text))
    return out


def states(c, run_id, card_id):
    try:
        page = c.call("RunInvocationsRequest", runId=run_id, card=path(*card_id.split("/")))
    except RpcFailure as failure:
        if failure.kind != "not_found":
            raise
        return []
    return [r.state for r in page.records]


@pytest.fixture
def env(tmp_path: Path):
    with ExitStack() as stack:
        count = 0

        def make(cards, conns, defs=None):
            nonlocal count
            root = tmp_path / f"project-{count}"
            count += 1
            write(root, defs or BASE_DEFS, cards, conns)
            return stack.enter_context(Daemon(root)).connect()

        yield make


def test_any_consumes_in_arrival_order_without_loss_or_duplicates(env):
    """A merge consumes each arriving value once and preserves arrival order."""
    c = env([card("src", "msg"), card("spread", "fork"), card("join", "merge"), card("out", "log")],
            [conn("src", "msg", "spread", "in"), conn("spread", "out1", "join", "a"),
             conn("spread", "out2", "join", "b"), conn("join", "out", "out", "in")])
    run_id, st = run_and_wait(c, "src", timeout=10)
    assert st.state == RunState.succeeded
    lines = logs(c, run_id, "out")
    assert len(lines) == 2  # one per arrival, none lost or repeated
    kinds = [json.loads(x)["value_type"] for x in lines]
    assert kinds == ["a", "b"]  # fork emits out1 before out2


def test_all_takes_one_value_per_input_and_optional_output_does_not_fire(env):
    """A branch waits for both inputs and runs only its selected downstream path."""
    c = env([card("start", "trigger"), card("src", "msg"), card("cond", "flag"), card("gate", "branch"),
             card("yes", "log"), card("no", "log")],
            [conn("src", "msg", "gate", "value"), conn("cond", "flag", "gate", "condition"),
             conn("gate", "a", "yes", "in"), conn("gate", "b", "no", "in")])
    # Whole run from the trigger: every source starts once.
    run_id, st = run_and_wait(c, "start", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    assert states(c, run_id, "gate") == [CardState.succeeded]
    assert states(c, run_id, "yes") == [CardState.succeeded]
    assert states(c, run_id, "no") in ([], [CardState.skipped])  # b never fired
    # The record says which output carried the value and which stages ran.
    rec = c.call("RunInvocationsRequest", runId=run_id, card=path("gate")).records[0]
    assert [text(p) for p in rec.outputsSent] == ["a"]
    assert [(st.phase, st.outcome) for st in rec.stages] == [(Phase.execute, StageOutcome.done),
                                                             (Phase.validate, StageOutcome.done),
                                                             (Phase.save, StageOutcome.done)]
    assert all(st.start <= st.end for st in rec.stages)


def test_max_runs_is_enforced(env):
    """A card stops the run when another invocation would exceed maxRuns."""
    defs = dict(BASE_DEFS, merge={"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}], "maxRuns": 1})
    c = env([card("src", "msg"), card("spread", "fork"), card("join", "merge"), card("out", "log")],
            [conn("src", "msg", "spread", "in"), conn("spread", "out1", "join", "a"),
             conn("spread", "out2", "join", "b"), conn("join", "out", "out", "in")], defs)
    run_id, st = run_and_wait(c, "src", timeout=10)
    assert st.state == RunState.failed
    assert st.failure.kind == FailureKind.execution_limit
    assert CardState.failed in states(c, run_id, "join")


def subgraph(id_, inner_text_id="inner"):
    return card_in_card(id_, [card(inner_text_id, "fork")], [],
                        {"inputs": {"in": {"card": inner_text_id, "port": "in"}},
                         "outputs": {"out": {"card": inner_text_id, "port": "out1"}}})


def test_subgraph_runs_as_one_card_and_keeps_paths_apart(env):
    """Nested workflows keep separate histories and honor partial-run propagation."""
    c = env([card("src", "msg"), card("f", "fork"), subgraph("s1"), subgraph("s2"), card("o1", "log"), card("o2", "log")],
            [conn("src", "msg", "f", "in"), conn("f", "out1", "s1", "in"), conn("f", "out2", "s2", "in"),
             conn("s1", "out", "o1", "in"), conn("s2", "out", "o2", "in")])
    run_id, st = run_and_wait(c, "src", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    # Same inner id, different full paths: separate histories.
    assert states(c, run_id, "s1/inner") == [CardState.succeeded]
    assert states(c, run_id, "s2/inner") == [CardState.succeeded]
    # Run the subgraph alone from its boundary input (saved values).
    run2, st2 = run_and_wait(c, "s1", propagate=False, timeout=10)
    assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
    assert states(c, run2, "s1/inner") == [CardState.succeeded]
    assert states(c, run2, "o1") == []  # not propagated
    assert states(c, run2, "s2/inner") == []
    # With propagation the outside downstream runs too.
    run3, st3 = run_and_wait(c, "s1", propagate=True, timeout=10)
    assert st3.state == RunState.succeeded
    assert states(c, run3, "o1") == [CardState.succeeded]
    assert states(c, run3, "o2") == []


def test_subgraph_run_without_saved_inputs_fails_explicitly(env):
    """A partial workflow run reports when no saved boundary input exists."""
    c = env([card("src", "msg"), subgraph("s1"), card("o1", "log")],
            [conn("src", "msg", "s1", "in"), conn("s1", "out", "o1", "in")])
    _, st = run_and_wait(c, "s1", propagate=False, timeout=10)
    assert st.state == RunState.failed
    assert st.failure.kind == FailureKind.input_missing and "saved" in text(st.failure.diagnostic)


def set_param(c, card_id, name, value):
    snap = c.call("ProjectInspectRequest")
    body = next(json.loads(text(d.cardJson)) for d in snap.definitions if text(d.id) == card_id)
    next(p for p in body["params"] if p["name"] == name)["data"] = value
    c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
           changes=[change("PutDefinition", id=card_id, cardJson=json.dumps(body), keepMeta=True)])


FAN_DEFS = {**BASE_DEFS, "words": dict(BASE_DEFS["msg"]), "cond": dict(BASE_DEFS["flag"])}
FAN_CARDS = [card("start", "trigger"), card("fan", "fork"), card("words", "words"), card("cond", "cond"),
             card("gate", "branch"), card("yes", "log"), card("no", "log")]
FAN_CONNS = [conn("start", "out", "fan", "in"), conn("fan", "out1", "words", "start"), conn("fan", "out2", "cond", "start"),
             conn("words", "msg", "gate", "value"), conn("cond", "flag", "gate", "condition"),
             conn("gate", "a", "yes", "in"), conn("gate", "b", "no", "in")]


def test_run_from_one_branch_meets_the_saved_values_of_the_others(env):
    """A partial branch run joins its new value with the other saved inputs."""
    c = env(FAN_CARDS, FAN_CONNS, FAN_DEFS)
    first, st = run_and_wait(c, "start", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    assert "hello" in logs(c, first, "yes")[0]

    # Only this branch runs; the join takes its new value and the saved condition.
    set_param(c, "words", "text", "changed")
    run_id, st = run_and_wait(c, "words", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    assert states(c, run_id, "words") == [CardState.succeeded]
    assert states(c, run_id, "cond") == [] and states(c, run_id, "fan") == []
    assert states(c, run_id, "gate") == [CardState.succeeded]
    assert "changed" in logs(c, run_id, "yes")[0]
    # Only the branch that ran is a source; the record says where the saved value came from.
    rec = c.call("RunRecordRequest", runId=run_id, card=path("gate")).record
    assert [text(s.port) for s in rec.inputSources] == ["value"]
    journal = c.call("LogRangeRequest", runId=run_id, stream=3)
    assert any(first in line and "gate.condition" in line for line in text(journal.text).splitlines())

    # The other branch next: the saved value is now the one that just went through.
    set_param(c, "cond", "value", False)
    run_id, st = run_and_wait(c, "cond", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    assert states(c, run_id, "words") == []
    assert "changed" in logs(c, run_id, "no")[0]

    # The join alone reuses that set as it is.
    run_id, st = run_and_wait(c, "gate", propagate=False, timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    assert states(c, run_id, "gate") == [CardState.succeeded]


def test_run_from_one_branch_without_saved_values_says_so(env):
    """A partial branch reports a missing saved input and blocks its dependents."""
    c = env([card("words", "words"), card("cond", "cond"), card("gate", "branch"), card("yes", "log")],
            [conn("words", "msg", "gate", "value"), conn("cond", "flag", "gate", "condition"), conn("gate", "a", "yes", "in")],
            FAN_DEFS)
    run_id, st = run_and_wait(c, "words", timeout=10)
    assert st.state == RunState.failed
    assert st.failure.kind == FailureKind.input_missing and "saved inputs for gate" in text(st.failure.diagnostic)
    assert states(c, run_id, "words") == [CardState.succeeded]  # the branch itself still ran
    assert states(c, run_id, "yes") == [CardState.blocked]


def test_a_saved_value_never_goes_ahead_of_the_new_one_on_a_shared_port(env):
    """A running input source takes precedence over a saved value on the same port."""
    defs = {**FAN_DEFS, "other": dict(BASE_DEFS["msg"])}
    c = env(FAN_CARDS + [card("other", "other")],
            FAN_CONNS + [conn("other", "msg", "gate", "value")], defs)
    _, st = run_and_wait(c, "start", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    set_param(c, "words", "text", "changed")
    run_id, st = run_and_wait(c, "words", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    assert [("changed" in line) for line in logs(c, run_id, "yes")] == [True]


def test_run_from_one_branch_of_an_any_card_consumes_only_what_arrives(env):
    """An `any` Card takes values as they arrive, so nothing is added to them."""
    defs = {**FAN_DEFS, "other": dict(BASE_DEFS["msg"])}
    c = env([card("start", "trigger"), card("fan", "fork"), card("words", "words"), card("other", "other"),
             card("join", "merge"), card("out", "log")],
            [conn("start", "out", "fan", "in"), conn("fan", "out1", "words", "start"), conn("fan", "out2", "other", "start"),
             conn("words", "msg", "join", "a"), conn("other", "msg", "join", "b"), conn("join", "out", "out", "in")], defs)
    _, st = run_and_wait(c, "start", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    set_param(c, "words", "text", "changed")
    run_id, st = run_and_wait(c, "words", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    assert states(c, run_id, "join") == [CardState.succeeded]
    assert ["changed" in line for line in logs(c, run_id, "out")] == [True]


def test_start_wired_to_input_and_drafts_leave_only_their_dependents_out(env):
    """Draft cards block their dependents while implemented branches still run."""
    defs = dict(BASE_DEFS)
    defs["plan"] = {"template": "draft", "inputs": [{"name": "data"}]}
    c = env([card("start", "trigger"), card("src", "msg"), card("f", "fork"), card("out", "log"), card("plan", "plan"),
             card("after", "log")],
            [conn("start", "out", "src", "start"), conn("src", "msg", "f", "in"), conn("f", "out1", "out", "in"),
             conn("f", "out2", "plan", "data"),
             conn("plan", "summary", "after", "in")], defs)
    # The Draft's ports come from _meta.json (names and purposes only).
    snap = c.call("ProjectInspectRequest")
    meta = {"description": "Summarize", "content": "Input: messages. Output: the word count. Count the words.",
            "inputs": [{"name": "data", "description": "messages"}],
            "outputs": [{"name": "summary", "description": "the word count"}]}
    res = c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t", changes=[
        change("PutDefinition", id="plan", cardJson=json.dumps(defs["plan"]), metaJson=json.dumps(meta))])
    plan = next(r for r in res.delta.resolution if [text(s) for s in r.card.segments] == ["plan"])
    assert [text(p.name) for p in plan.outputs] == ["summary"]
    run_id, st = run_and_wait(c, "start", timeout=10)
    # The implemented part ran; the Draft and the Card that needs its output did not.
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    assert states(c, run_id, "src") == [CardState.succeeded]
    assert states(c, run_id, "out") == [CardState.succeeded]
    assert states(c, run_id, "plan") == [] and states(c, run_id, "after") == []
    # Step by step: running from the implemented Card leaves the Draft out too.
    run_id, st = run_and_wait(c, "src", timeout=10)
    assert st.state == RunState.succeeded and states(c, run_id, "out") == [CardState.succeeded]
    # A Draft itself cannot be run.
    snap = c.call("ProjectInspectRequest")
    with pytest.raises(RpcFailure):
        c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="t", card=path("plan"), propagate=True)


def test_merges_nested_several_levels_deep(env):
    """Nested merge values preserve their union names through four graph levels."""
    cards = [card("start", "trigger"), card("src", "msg"), card("out", "log")]
    conns = [conn("start", "out", "src", "start")]
    prev = ("src", "msg")
    for i in range(4):
        cards += [card(f"f{i}", "fork"), card(f"m{i}", "merge")]
        conns += [conn(prev[0], prev[1], f"f{i}", "in"), conn(f"f{i}", "out1", f"m{i}", "a")]
        prev = (f"m{i}", "out")
    conns.append(conn(prev[0], prev[1], "out", "in"))
    c = env(cards, conns, defs=dict(BASE_DEFS, merge={"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}]}))
    run_id, st = run_and_wait(c, "start", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    out = json.loads(logs(c, run_id, "out")[0])
    for _ in range(4):
        assert out["value_type"] == "a"
        out = out["value"]
    assert out == {"text": "hello"}
