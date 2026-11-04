"""A project's versions are a git repository: a run commits what is saved
(when it changed) and records that commit, its result goes to the commit's
note, a named version is an annotated tag, starting from an older version
opens a branch, and a demo's history file becomes its versions."""

from __future__ import annotations

import json
import shutil
import subprocess
import time

from conftest import ROOT, Daemon
from projects import builtin_project, run_and_wait
from tsugumi_client import _request_class, change, new_id, text
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
        assert src_text(c) == "final"
        assert git(root, "show", "HEAD~1:definitions/src/_card.json").count("tried") == 1
    finally:
        d.cleanup()


def test_the_casa_demo_carries_its_trials(tmp_path):
    """The Cas A demo's copy becomes a repository with how it came to be:
    branches tried, a failed run, the adopted version tagged, and the
    spectrum added on top of it."""
    root = tmp_path / "casa"
    shutil.copytree(ROOT / "demos" / "casa", root)
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
        assert text(head.message).startswith("spectrum を追加") and not (head.tags or [])
        adopted = by_id[text(head.parents[0])]
        assert [text(t) for t in adopted.tags] == ["rgb-v1"]
        assert "failed" not in git(root, "log", "--notes=tsugumi", "--format=%N", "main", "-1")
        assert git(root, "log", "--notes=tsugumi", "--grep=failed", "--all", "--format=%s").startswith("青の帯域")
        # Going back to the plan gives the Drafts.
        plan = next(x for x in g.commits if not (x.parents or []))
        rev = c.call("ProjectInspectRequest").rev
        c.call("VersionCheckoutRequest", baseRev=rev, opId=new_id(), origin="test", commit=text(plan.id))
        snap = c.call("ProjectInspectRequest")
        assert all(json.loads(text(d.cardJson)).get("template") in ("draft", "trigger") for d in snap.definitions)
    finally:
        d.cleanup()
