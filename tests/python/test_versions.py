"""A project's versions are a git repository: a run commits what is saved
(when it changed) and records that commit, its result goes to the commit's
note, a named version is an annotated tag, starting from an older version
opens a branch, and a demo's history file becomes its versions. A run's line
in the note says what ran, from what the run did."""

from __future__ import annotations

import json
import shutil
import subprocess
import time

from conftest import ROOT, Daemon
from projects import MSG_FBS, builtin_project, card, conn, run_and_wait, write
from tsugumi_client import RpcFailure, _request_class, change, new_id, text
from Tsugumi.Data.RunState import RunState


def git(root, *args) -> str:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, check=True).stdout


def graph(c):
    return c.call("VersionGraphRequest", limit=100)


def notes(c, commit: str, n: int, timeout=10.0) -> list:
    """The runs a commit's note lists, once it lists n (written as runs end)."""
    deadline = time.monotonic() + timeout
    while True:
        g = graph(c)
        runs = next(x.runs or [] for x in g.commits if text(x.id) == commit)
        if len(runs) >= n or time.monotonic() > deadline:
            return runs
        time.sleep(0.1)


def set_text(c, value: str):
    snap = c.call("ProjectInspectRequest")
    body = next(json.loads(text(d.cardJson)) for d in snap.definitions if text(d.id) == "src")
    next(p for p in body["params"] if p["name"] == "text")["data"] = value
    c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="test",
           changes=[change("PutDefinition", id="src", cardJson=json.dumps(body), keepMeta=True)])


def src_text(c) -> str:
    snap = c.call("ProjectInspectRequest")
    body = next(json.loads(text(d.cardJson)) for d in snap.definitions if text(d.id) == "src")
    return next(p for p in body["params"] if p["name"] == "text")["data"]


def test_runs_commit_note_and_branch(tmp_path):
    root = tmp_path / "p"
    builtin_project(root, text="first")
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        assert not graph(c).enabled
        # A run of a project that is not versioned records no version.
        _, st = run_and_wait(c, "start")
        assert st.state == RunState.succeeded and not st.commit
        init = c.call("VersionInitRequest", opId=new_id(), origin="test")
        first = text(init.commit)
        assert git(root, "log", "-1", "--format=%s").strip() == "プロジェクトを作成"  # a Japanese connection
        assert ".tsugumi/" in (root / ".gitignore").read_text()
        g = graph(c)
        assert g.enabled and text(g.head) == first and not g.dirty
        # A run after a change commits it; a run without one shares the commit.
        set_text(c, "second")
        assert graph(c).dirty
        run1, st1 = run_and_wait(c, "start")
        run2, st2 = run_and_wait(c, "start")
        second = text(st1.commit)
        assert second and second != first and text(st2.commit) == second
        runs = notes(c, second, 2)
        assert [text(r.runId) for r in runs] == [run1, run2] and all(text(r.state) == "succeeded" for r in runs)
        changed = next(x for x in graph(c).commits if text(x.id) == second).changed
        assert [text(k) for k in changed] == ["src"]
        # git selects versions by their runs' results, and names them by tags.
        assert second in git(root, "log", "--notes=tsugumi", "--grep=succeeded", "--format=%H")
        # (the request's own field is `name`, as call()'s and make()'s first argument)
        mark = _request_class("VersionMarkRequest")()
        mark.opId, mark.origin, mark.name, mark.message = new_id(), "test", "second-text", "二つ目の文"
        c.wait_reply(c.send("VersionMarkRequest", mark))
        assert git(root, "tag", "-l").split() == ["second-text"]
        # Starting from the first version: the documents are that version's,
        # and a change there opens a branch; the later one stays.
        rev = c.call("ProjectInspectRequest").rev
        c.call("VersionCheckoutRequest", baseRev=rev, opId=new_id(), origin="test", commit=first)
        assert src_text(c) == "first"
        set_text(c, "third")
        _, st3 = run_and_wait(c, "start")
        g = graph(c)
        branches = {text(b) for x in g.commits for b in (x.branches or [])}
        assert text(g.branch).startswith("try-") and len(branches) == 2, branches
        third = next(x for x in g.commits if text(x.id) == text(st3.commit))
        assert [text(p) for p in third.parents] == [first]
    finally:
        d.cleanup()


def test_a_demo_history_becomes_its_versions(tmp_path):
    root = tmp_path / "p"
    builtin_project(root, text="final")
    card = json.loads((root / "definitions" / "src" / "_card.json").read_text())
    tried = dict(card, params=[dict(p, data="tried") if p["name"] == "text" else p for p in card["params"]])
    history = {
        "commits": [
            {"id": "plan", "message": "計画", "time": 1793700000, "files": {"definitions/src/_card.json": json.dumps(tried)}},
            {"id": "try", "parents": ["plan"], "message": "別の文を試す", "time": 1793700600,
             "runs": [{"run_id": "11111111-1111-1111-1111-111111111111", "state": "failed", "ended": "2026-11-04T01:10:00Z", "seconds": 2.5}]},
            {"id": "final", "parents": ["plan"], "message": "採用", "time": 1793701200, "tag": "adopted",
             "runs": [{"run_id": "22222222-2222-2222-2222-222222222222", "state": "succeeded", "ended": "2026-11-04T01:20:00Z", "seconds": 1.2}]},
        ],
        "branches": {"main": "final", "try-other-text": "try"},
        "head": "main",
    }
    (root / ".tsugumi-history.json").write_text(json.dumps(history))
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        c.call("VersionInitRequest", opId=new_id(), origin="test")
        assert not (root / ".tsugumi-history.json").exists()
        g = graph(c)
        assert text(g.branch) == "main" and not g.dirty
        by = {text(x.summary): x for x in g.commits}
        assert set(by) == {"計画", "別の文を試す", "採用"}
        assert [text(t) for t in by["採用"].tags] == ["adopted"]
        assert [text(r.state) for r in by["別の文を試す"].runs] == ["failed"]
        assert not by["別の文を試す"].runs[0].summary  # the history file gives none
        assert src_text(c) == "final"
        assert git(root, "show", "HEAD~1:definitions/src/_card.json").count("tried") == 1
    finally:
        d.cleanup()


def test_the_casa_demo_carries_its_trials(tmp_path):
    """The Cas A demo's copy becomes a repository with how it came to be:
    branches tried, a failed run, the adopted version tagged, the spectrum
    added on top of it, and then CIAO."""
    root = tmp_path / "casa"
    shutil.copytree(ROOT / "demos" / "ja" / "casa", root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        c.call("VersionInitRequest", opId=new_id(), origin="test")
        g = graph(c)
        assert not g.dirty and text(g.branch) == "main"
        assert {text(b) for x in g.commits for b in (x.branches or [])} == {"main", "try-hard-blue", "try-soft-green"}
        states = [text(r.state) for x in g.commits for r in (x.runs or [])]
        assert "failed" in states and states.count("succeeded") >= 5
        by_id = {text(x.id): x for x in g.commits}
        head = by_id[text(g.head)]
        assert text(head.message).startswith("CIAO を固定") and not (head.tags or [])
        spectrum = by_id[text(head.parents[0])]
        assert text(spectrum.message).startswith("spectrum を追加")
        adopted = by_id[text(spectrum.parents[0])]
        assert [text(t) for t in adopted.tags] == ["rgb-v1"]
        assert "failed" not in git(root, "log", "--notes=tsugumi", "--format=%N", "main", "-1")
        # The versions before CIAO are as they were: no Card of it, bands without dmcopy.
        assert "definitions/ciao/_card.json" not in git(root, "ls-tree", "-r", "--name-only", text(spectrum.id))
        assert "dmcopy" not in git(root, "show", f"{text(spectrum.id)}:definitions/bands/src/main.py")
        assert git(root, "log", "--notes=tsugumi", "--grep=failed", "--all", "--format=%s").startswith("青の帯域")
        # Going back to the plan gives the Drafts.
        plan = next(x for x in g.commits if not (x.parents or []))
        rev = c.call("ProjectInspectRequest").rev
        c.call("VersionCheckoutRequest", baseRev=rev, opId=new_id(), origin="test", commit=text(plan.id))
        snap = c.call("ProjectInspectRequest")
        assert all(json.loads(text(d.cardJson)).get("template") in ("draft", "trigger") for d in snap.definitions)
    finally:
        d.cleanup()


def note_lines(root, commit: str) -> list[str]:
    return git(root, "notes", "--ref=tsugumi", "show", commit).splitlines()


def summary_of(c, root, st) -> str:
    """The summary of the run that just ended: on the last line of its commit's
    note, and the same through the version graph."""
    commit = text(st.commit)
    deadline = time.monotonic() + 10.0
    while True:
        runs = next(x.runs or [] for x in graph(c).commits if text(x.id) == commit)
        if any(text(r.runId) == text(st.runId) for r in runs) or time.monotonic() > deadline:
            break
        time.sleep(0.1)
    run = next(r for r in runs if text(r.runId) == text(st.runId))
    line = next(x for x in note_lines(root, commit) if x.startswith(f"run {text(st.runId)} "))
    # run <id> <state> <ended> <seconds> <summary>
    assert line.split(" ", 5)[5] == text(run.summary), line
    return text(run.summary)


def test_a_run_note_says_what_ran(tmp_path):
    """A run's note says, from what the run did: where it started, the Cards
    that ran (how many times, past one), and those that failed or never ran.
    What did not happen is not there, and a succeeded run's note stays out of
    git log --grep=failed."""
    root = tmp_path / "p"
    write(root,
          defs={"trigger": {"template": "trigger"},
                "source": {"template": "input", "params": [{"name": "text", "type": "string", "data": "first"}], "_outputs": MSG_FBS},
                "fork": {"template": "fork"},
                "merge": {"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}], "maxRuns": 4},
                "log": {"template": "logger", "maxRuns": 4}},
          cards=[card("start", "trigger"), card("src", "source"), card("split", "fork"), card("both", "merge"), card("sink", "log")],
          conns=[conn("src", "msg", "split", "in"), conn("split", "out1", "both", "a"),
                 conn("split", "out2", "both", "b"), conn("both", "out", "sink", "in")])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        c.call("VersionInitRequest", opId=new_id(), origin="test")
        # On its saved inputs before there are any: it fails without running,
        # and the Cards after it never run.
        _, st = run_and_wait(c, "both")
        assert st.state == RunState.failed
        assert summary_of(c, root, st) == "both から / 失敗: both（input_missing） / 未実行: sink"
        # The whole Workflow: the trigger is no step of its own, and the merge
        # and what follows ran once per branch.
        set_text(c, "second")
        _, st = run_and_wait(c, "start")
        assert st.state == RunState.succeeded
        whole = text(st.commit)
        assert summary_of(c, root, st) == "全体: src、split、both ×2、sink ×2"
        assert whole not in git(root, "log", "--notes=tsugumi", "--grep=failed", "--format=%H", "-1")
        # A Card and what follows it, then one Card alone, on the saved inputs.
        _, st = run_and_wait(c, "split")
        assert summary_of(c, root, st) == "split から（保存済みの入力で）: split、both ×2、sink ×2"
        _, st = run_and_wait(c, "sink", propagate=False)
        assert summary_of(c, root, st) == "sink だけ（保存済みの入力で）: sink"
        # Runs without a change share the commit: each has its own line.
        lines = note_lines(root, whole)
        assert len(lines) == 3 and all(x.startswith("run ") for x in lines)
    finally:
        d.cleanup()


def test_a_run_is_recorded_in_the_language_of_the_connection_that_started_it(tmp_path):
    """What tsugumid writes into the project's git (the first commit's message,
    and for a run the commit's message and the note) is in the language the starting connection's
    SessionHello asked for: English for "en", Japanese otherwise."""
    root = tmp_path / "p"
    write(root,
          defs={"trigger": {"template": "trigger"},
                "source": {"template": "input", "params": [{"name": "text", "type": "string", "data": "first"}], "_outputs": MSG_FBS},
                "fork": {"template": "fork"},
                "merge": {"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}], "maxRuns": 4},
                "log": {"template": "logger", "maxRuns": 4}},
          cards=[card("start", "trigger"), card("src", "source"), card("split", "fork"), card("both", "merge"), card("sink", "log")],
          conns=[conn("src", "msg", "split", "in"), conn("split", "out1", "both", "a"),
                 conn("split", "out2", "both", "b"), conn("both", "out", "sink", "in")])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello(language="en")
        c.call("VersionInitRequest", opId=new_id(), origin="test")
        assert git(root, "log", "-1", "--format=%s").strip() == "Create the project"
        _, st = run_and_wait(c, "both")
        assert summary_of(c, root, st) == "from both / failed: both (input_missing) / not run: sink"
        set_text(c, "second")
        _, st = run_and_wait(c, "start")
        assert summary_of(c, root, st) == "whole: src, split, both ×2, sink ×2"
        assert git(root, "log", "-1", "--format=%s", text(st.commit)).strip() == "Run: start"
        _, st = run_and_wait(c, "split")
        assert summary_of(c, root, st) == "from split (with saved inputs): split, both ×2, sink ×2"
        _, st = run_and_wait(c, "sink", propagate=False)
        assert summary_of(c, root, st) == "only sink (with saved inputs): sink"
        # Another connection, in Japanese, on the same project.
        j = d.client()
        j.hello()
        set_text(j, "third")
        _, st = run_and_wait(j, "start")
        assert summary_of(j, root, st) == "全体: src、split、both ×2、sink ×2"
        assert git(root, "log", "-1", "--format=%s", text(st.commit)).strip() == "実行: start"
    finally:
        d.cleanup()


def test_a_run_note_names_a_card_that_ran_and_failed(tmp_path):
    """The merge's second invocation fails (maxRuns): the record has both
    invocations, and what follows ran on the first one's value."""
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("feed", "source"), card("split", "fork"), card("both", "merge"), card("sink", "log")],
                    extra_conns=[conn("feed", "msg", "split", "in"), conn("split", "out1", "both", "a"),
                                 conn("split", "out2", "both", "b"), conn("both", "out", "sink", "in")])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        c.call("VersionInitRequest", opId=new_id(), origin="test")
        _, st = run_and_wait(c, "feed")
        assert st.state == RunState.failed
        summary = summary_of(c, root, st)
        assert summary == "feed から: feed、split、both ×2、sink / 失敗: both（execution_limit）", summary
        assert text(st.commit) in git(root, "log", "--notes=tsugumi", "--grep=failed", "--format=%H", "-1")
    finally:
        d.cleanup()


def test_a_long_run_summary_stays_short(tmp_path):
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card(f"more{i}", "source") for i in range(8)])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        c.call("VersionInitRequest", opId=new_id(), origin="test")
        _, st = run_and_wait(c, "start")
        assert st.state == RunState.succeeded
        assert summary_of(c, root, st).endswith(" ほか 4")
    finally:
        d.cleanup()


def test_a_versions_canvas_says_what_each_card_is(tmp_path):
    """A minimap draws what is not a Card with its own shape: the canvas of a
    version says what each Card is, how wide a fork or merge is, which way the
    Cards run and which are folded, all as that version has them."""
    def commit_all(project):  # these Cards are not wired to run: a version of them is committed by hand
        git(project, "add", "-A")
        git(project, "-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "--allow-empty", "-qm", "next")
        return git(project, "rev-parse", "HEAD").strip()

    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("f", "fork"), card("wide", "fork"), card("b", "branch"), card("m", "merge")],
                    extra_conns=[conn("src", "msg", "m", "a")])
    wide = root / "definitions" / "wide" / "_card.json"
    wide.write_text(json.dumps({"template": "fork", "params": [{"name": "branches", "type": "string[]", "data": ["a", "b", "c"]}]}))
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        first = text(c.call("VersionInitRequest", opId=new_id(), origin="test").commit)
        canvas = c.call("VersionCanvasRequest", commit=first)
        cards = {text(k.id): k for k in canvas.cards}
        assert {k: text(v.kind) for k, v in cards.items()} == {
            "start": "trigger", "src": "input", "out": "logger", "f": "fork", "wide": "fork", "b": "branch", "m": "merge"}
        assert (cards["f"].fan, cards["wide"].fan, cards["m"].fan, cards["b"].fan, cards["out"].fan) == (2, 3, 2, 0, 0)
        # Columns by depth run left to right, whatever the layout says.
        assert text(canvas.flow) == "leftToRight" and not any(k.collapsed for k in canvas.cards)

        at = {k: {"x": 0, "y": i * 100} for i, k in enumerate(cards)}
        at["out"]["collapsed"] = True
        layouts = [
            # The saved direction, and without one, from where the wired Cards sit.
            ({"flow": "topToBottom", "cards": {k: {**v, "x": i * 100, "y": 0} for i, (k, v) in enumerate(at.items())}}, "topToBottom"),
            ({"flow": "leftToRight", "cards": at}, "leftToRight"),
            ({"cards": at}, "topToBottom"),
            ({"cards": {k: {**v, "x": i * 100, "y": 0} for i, (k, v) in enumerate(at.items())}}, "leftToRight"),
        ]
        for layout, flow in layouts:
            (root / "layout.json").write_text(json.dumps(layout))
            placed = c.call("VersionCanvasRequest", commit=commit_all(root))
            assert text(placed.flow) == flow, layout
            assert {text(k.id) for k in placed.cards if k.collapsed} == {"out"}
        # An older version keeps what its Cards were.
        wide.write_text(json.dumps({"template": "merge", "inputs": [{"name": "a"}]}))
        now = {text(k.id): k for k in c.call("VersionCanvasRequest", commit=commit_all(root)).cards}
        assert (text(now["wide"].kind), now["wide"].fan) == ("merge", 1)
        then = {text(k.id): k for k in c.call("VersionCanvasRequest", commit=first).cards}
        assert (text(then["wide"].kind), then["wide"].fan) == ("fork", 3)
    finally:
        d.cleanup()


def test_a_version_has_its_canvas_diff_and_summary(tmp_path):
    """A version's Cards and wires are read from that version; its diff has the
    code and settings a summary may read and never data; a summary goes to a
    note of its own, so the runs' note and git log --grep=failed are untouched."""
    root = tmp_path / "p"
    builtin_project(root, text="first",
                    extra_cards=[card("later", "source")])
    (root / "definitions" / "src" / "data").mkdir()
    (root / "definitions" / "src" / "data" / "secret.csv").write_text("patient,1\n")
    (root / "definitions" / "src" / "src").mkdir()
    (root / "definitions" / "src" / "src" / "values.csv").write_text("patient,2\n")
    (root / "definitions" / "src" / "src" / "main.py").write_text("print('code one')\n")
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        first = text(c.call("VersionInitRequest", opId=new_id(), origin="test").commit)
        # Everything in the first version, but only what a summary may read.
        whole = text(c.call("VersionDiffRequest", commit=first).text)
        assert "print('code one')" in whole and '"text"' in whole
        assert "patient" not in whole and "secret" not in whole and "values.csv" not in whole
        # No saved positions: columns by depth, and only real wires.
        canvas = c.call("VersionCanvasRequest", commit=first)
        at = {text(k.id): (k.x, k.y) for k in canvas.cards}
        assert set(at) == {"start", "src", "out", "later"}
        assert [(text(w.from_), text(w.to)) for w in canvas.wires] == [("src", "out")]
        assert at["src"][0] < at["out"][0]
        # A later version: its diff is what it changed, and the older version
        # keeps its own Cards.
        set_text(c, "second")
        (root / "definitions" / "src" / "data" / "secret.csv").write_text("patient,3\n")
        (root / "layout.json").write_text(json.dumps({"cards": {k: {"x": i * 100, "y": 7} for i, k in enumerate(at)}}))
        c.call("ProjectReloadRequest", opId=new_id(), origin="test", baseRev=c.call("ProjectInspectRequest").rev)
        _, st = run_and_wait(c, "start")
        second = text(st.commit)
        change = c.call("VersionDiffRequest", commit=second)
        assert '"second"' in text(change.text) and "first" in text(change.text) and "patient" not in text(change.text)
        assert "layout.json" not in text(change.text) and not change.truncated
        placed = {text(k.id): (k.x, k.y) for k in c.call("VersionCanvasRequest", commit=second).cards}
        assert placed["src"][1] == 7 and placed["out"][0] == 200
        # The summary: on the commit, in its own note, and replaced when written again.
        assert not next(x for x in graph(c).commits if text(x.id) == second).changeSummary
        for words in ("一度目", "failed が出ないように直した"):
            c.call("VersionSummaryRequest", opId=new_id(), origin="test", commit=second, summary=words)
        got = next(x for x in graph(c).commits if text(x.id) == second)
        assert text(got.changeSummary) == "failed が出ないように直した"
        assert git(root, "notes", "--ref=tsugumi-summary", "show", second).strip() == "failed が出ないように直した"
        notes(c, second, 1)
        assert second not in git(root, "log", "--notes=tsugumi", "--grep=failed", "--format=%H")
        # A commit that is not there is refused, and nothing is kept.
        try:
            c.call("VersionCanvasRequest", commit="0" * 40)
            raise AssertionError("no such version")
        except RpcFailure as e:
            assert e.kind == "not_found"
    finally:
        d.cleanup()
