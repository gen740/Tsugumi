"""Scheduler behaviour with builtin Cards only (no Nix): consumption order of
all/any inputs, non-firing optional outputs, maxRuns, no lost or duplicated
tokens, subgraph runs through their boundary, and per-path history."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from conftest import Daemon
from projects import card, conn
from test_regressions import run_and_wait, write
from tsugumi_client import RpcFailure, path, text
from Tsugumi.Data.CardState import CardState
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
    except RpcFailure:
        return []
    return [r.state for r in page.records]


@pytest.fixture
def env(tmp_path: Path):
    started = []

    def make(cards, conns, defs=None):
        root = tmp_path / f"p{len(started)}"
        write(root, defs or BASE_DEFS, cards, conns)
        d = Daemon(root).start()
        started.append(d)
        c = d.client()
        c.hello()
        return c

    yield make
    for d in started:
        d.cleanup()


def test_any_consumes_in_arrival_order_without_loss_or_duplicates(env):
    c = env([card("src", "msg"), card("spread", "fork"), card("join", "merge"), card("out", "log")],
            [conn("src", "msg", "spread", "in"), conn("spread", "out1", "join", "a"),
             conn("spread", "out2", "join", "b"), conn("join", "out", "out", "in")])
    run_id, st = run_and_wait(c, "src")
    assert st.state == RunState.succeeded
    lines = logs(c, run_id, "out")
    assert len(lines) == 2  # one per arrival, none lost or repeated
    kinds = [json.loads(x)["value_type"] for x in lines]
    assert kinds == ["a", "b"]  # fork emits out1 before out2


def test_all_takes_one_value_per_input_and_optional_output_does_not_fire(env):
    c = env([card("begin", "trigger"), card("src", "msg"), card("cond", "flag"), card("gate", "branch"),
             card("yes", "log"), card("no", "log")],
            [conn("src", "msg", "gate", "value"), conn("cond", "flag", "gate", "condition"),
             conn("gate", "a", "yes", "in"), conn("gate", "b", "no", "in")])
    # Whole run from the trigger: every source starts once.
    run_id, st = run_and_wait(c, "begin")
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    assert states(c, run_id, "gate") == [CardState.succeeded]
    assert states(c, run_id, "yes") == [CardState.succeeded]
    assert states(c, run_id, "no") in ([], [CardState.skipped])  # b never fired


def test_max_runs_is_enforced(env):
    defs = dict(BASE_DEFS, merge={"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}], "maxRuns": 1})
    c = env([card("src", "msg"), card("spread", "fork"), card("join", "merge"), card("out", "log")],
            [conn("src", "msg", "spread", "in"), conn("spread", "out1", "join", "a"),
             conn("spread", "out2", "join", "b"), conn("join", "out", "out", "in")], defs)
    run_id, st = run_and_wait(c, "src")
    assert st.state == RunState.failed
    assert st.failure.kind == 19  # execution_limit
    assert CardState.failed in states(c, run_id, "join")


def subgraph(id_, inner_text_id="inner"):
    return card(id_, None, canvas={"cards": [card(inner_text_id, "fork")], "connections": []},
                boundary={"inputs": {"in": {"card": inner_text_id, "port": "in"}},
                          "outputs": {"out": {"card": inner_text_id, "port": "out1"}}})


def test_subgraph_runs_as_one_card_and_keeps_paths_apart(env):
    c = env([card("src", "msg"), subgraph("s1"), subgraph("s2"), card("o1", "log"), card("o2", "log")],
            [conn("src", "msg", "s1", "in"), conn("src", "msg", "s2", "in"),
             conn("s1", "out", "o1", "in"), conn("s2", "out", "o2", "in")])
    run_id, st = run_and_wait(c, "src")
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    # Same inner id, different full paths: separate histories.
    assert states(c, run_id, "s1/inner") == [CardState.succeeded]
    assert states(c, run_id, "s2/inner") == [CardState.succeeded]
    # Run the subgraph alone from its boundary input (saved values).
    run2, st2 = run_and_wait(c, "s1", propagate=False)
    assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
    assert states(c, run2, "s1/inner") == [CardState.succeeded]
    assert states(c, run2, "o1") == []  # not propagated
    assert states(c, run2, "s2/inner") == []
    # With propagation the outside downstream runs too.
    run3, st3 = run_and_wait(c, "s1", propagate=True)
    assert st3.state == RunState.succeeded
    assert states(c, run3, "o1") == [CardState.succeeded]
    assert states(c, run3, "o2") == []


def test_subgraph_run_without_saved_inputs_fails_explicitly(env):
    c = env([card("src", "msg"), subgraph("s1"), card("o1", "log")],
            [conn("src", "msg", "s1", "in"), conn("s1", "out", "o1", "in")])
    _, st = run_and_wait(c, "s1", propagate=False)
    assert st.state == RunState.failed
    assert st.failure.kind == 18 and "saved" in text(st.failure.diagnostic)
