"""A project's versions are a git repository: a run commits what is saved
(when it changed) and records that commit, its result goes to the commit's
note, a named version is an annotated tag, starting from an older version
opens a branch, and a demo's history file becomes its versions. A run's line
in the note says what ran, from what the run did."""

from __future__ import annotations

import json
import subprocess

import pytest

from harness.demos import copy_demo
from harness.file_events import wait_for_file
from projects import SINK, MSG_FBS, MSG_SOURCE_NIX, builtin_project, card, conn, run_and_wait, write, write_nix_project
from harness.document import card_document, params_nix, set_param
from tsugumi_client import RpcFailure, _request_class, change, new_id, path, text
from Tsugumi.Data.RunState import RunState
from harness.document import build_buffer, positions, write_layout


def git(root, *args) -> str:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, check=True).stdout


def graph(c):
    return c.call("VersionGraphRequest", limit=100)


def notes(c, root, commit: str, n: int = 0, *, run_ids=()) -> list:
    """Wait for the Git note ref to be published by the run reaper."""
    def read():
        runs = next(x.runs or [] for x in graph(c).commits if text(x.id) == commit)
        if len(runs) >= n and set(run_ids) <= {text(r.runId) for r in runs}:
            return runs
        return None

    return wait_for_file(root / ".git/refs/notes/tsugumi", read)


def set_text(c, value: str):
    set_param(c, "src", "text", value)


def src_text(c) -> str:
    snap = c.call("ProjectInspectRequest")
    node = next(r for r in snap.resolution if r.card.segments == [b"src"])
    return json.loads(text(next(p.valueJson for p in node.parameters if p.name == b"text")))


def test_runs_commit_note_and_branch(tmp_path, core_factory):
    """Runs record their saved version and checkout can start a separate branch."""
    root = tmp_path / "p"
    builtin_project(root, text="first")
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    assert not graph(c).enabled
    # A run of a project that is not versioned records no version.
    _, st = run_and_wait(c, "start")
    assert st.state == RunState.succeeded and not st.commit
    init = c.call("VersionInitRequest", opId=new_id(), origin="test")
    first = text(init.commit)
    created = graph(c).commits[0]
    assert not created.message and text(created.facts.kind) == "create" and not created.facts.card
    g = graph(c)
    assert g.enabled and text(g.head) == first and not g.dirty
    # A run after a change commits it; a run without one shares the commit.
    set_text(c, "second")
    assert graph(c).dirty
    run1, st1 = run_and_wait(c, "start")
    run2, st2 = run_and_wait(c, "start")
    second = text(st1.commit)
    assert second and second != first and text(st2.commit) == second
    runs = notes(c, root, second, run_ids=(run1, run2))
    assert {text(r.runId): text(r.state) for r in runs} == {run1: "succeeded", run2: "succeeded"}
    changed = next(x for x in graph(c).commits if text(x.id) == second).changed
    assert [text(k) for k in changed] == ["src"]
    # git selects versions by their runs' results, and names them by tags.
    # (the request's own field is `name`, as call()'s and make()'s first argument)
    mark = _request_class("VersionMarkRequest")()
    mark.opId, mark.origin, mark.name, mark.message = new_id(), "test", "second-text", "二つ目の文"
    c.wait_reply(c.send("VersionMarkRequest", mark))
    assert [text(tag) for entry in graph(c).commits for tag in (entry.tags or [])] == ["second-text"]
    # Starting from the first version: the documents are that version's,
    # and a change there opens a branch; the later one stays.
    original_branch = text(graph(c).branch)
    rev = c.call("ProjectInspectRequest").rev
    c.call("VersionCheckoutRequest", baseRev=rev, opId=new_id(), origin="test", commit=first)
    assert src_text(c) == "first"
    set_text(c, "third")
    _, st3 = run_and_wait(c, "start")
    g = graph(c)
    branches = {text(b) for x in g.commits for b in (x.branches or [])}
    assert text(g.branch) != original_branch and {original_branch, text(g.branch)} <= branches, branches
    third = next(x for x in g.commits if text(x.id) == text(st3.commit))
    assert [text(p) for p in third.parents] == [first]


def test_checkout_rejects_removed_definition_format_before_committing(tmp_path, core_factory):
    root = tmp_path / "p"
    builtin_project(root)
    c = core_factory(project_root=root).connect()
    head = text(c.call("VersionInitRequest", opId=new_id(), origin="test").commit)
    branch = text(graph(c).branch)
    git(root, "switch", "-c", "legacy")
    definition = root / "definitions/src/_card.json"
    definition.unlink()
    legacy_link = definition.with_name("_link.json")
    legacy_link.write_text('{"type":"subgraph","external":"old"}')
    git(root, "add", "-A")
    git(root, "-c", "user.name=Test", "-c", "user.email=test@example.test", "commit", "-m", "legacy")
    legacy = git(root, "rev-parse", "HEAD").strip()
    git(root, "switch", branch)

    set_text(c, "saved but uncommitted")
    before = definition.read_bytes()
    snapshot = c.call("ProjectInspectRequest")
    with pytest.raises(RpcFailure) as error:
        c.call("VersionCheckoutRequest", baseRev=snapshot.rev, opId=new_id(),
               origin="test", commit=legacy)
    assert error.value.kind == "invalid_document"
    assert git(root, "rev-parse", "HEAD").strip() == head
    assert git(root, "branch", "--show-current").strip() == branch
    assert definition.read_bytes() == before and not legacy_link.exists()
    assert c.call("ProjectInspectRequest").rev == snapshot.rev


@pytest.mark.parametrize("blocker", ["untracked-link", "index.lock", "HEAD.lock"])
def test_checkout_failure_preserves_the_project(tmp_path, core_factory, blocker):
    root = tmp_path / "p"
    builtin_project(root, text="target")
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    target = text(c.call("VersionInitRequest", opId=new_id(), origin="test").commit)
    set_text(c, "current")
    before = c.call("ProjectInspectRequest")
    head = (root / ".git/HEAD").read_bytes()
    definition = root / "definitions/src/_card.json"
    current = definition.read_bytes()
    # The user's index is distinct from the saved worktree.
    staged = root / "staged.txt"
    staged.write_text("stage only")
    git(root, "add", "staged.txt")
    index = (root / ".git/index").read_bytes()
    if blocker == "untracked-link":
        obstacle = definition.with_name("untracked-link")
        obstacle.symlink_to("_card.json")
    else:
        obstacle = root / ".git" / blocker
        obstacle.touch()

    with pytest.raises(RpcFailure) as error:
        c.call("VersionCheckoutRequest", baseRev=before.rev, opId=new_id(),
               origin="test", commit=target)
    assert error.value.kind == (
        "invalid_path" if blocker == "untracked-link" else "persistence_failed"
    )
    assert (root / ".git/HEAD").read_bytes() == head
    assert git(root, "rev-parse", "HEAD").strip() == target
    assert (root / ".git/index").read_bytes() == index
    assert definition.read_bytes() == current
    assert staged.read_text() == "stage only"
    assert c.call("ProjectInspectRequest").rev == before.rev

    obstacle.unlink()
    daemon.close()
    reopened = core_factory(project_root=root).connect()
    assert src_text(reopened) == "current"
    reopened.call("VersionCheckoutRequest",
                  baseRev=reopened.call("ProjectInspectRequest").rev,
                  opId=new_id(), origin="test", commit=target)
    assert src_text(reopened) == "target"
    assert staged.read_text() == "stage only"


def test_checkout_crash_restores_worktree_index_head_and_branch(tmp_path, core_factory):
    root = tmp_path / "p"
    builtin_project(root, text="target")
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    target = text(c.call("VersionInitRequest", opId=new_id(), origin="test").commit)
    git(root, "switch", "-c", "original")
    set_text(c, "current")
    git(root, "add", "definitions")
    git(root, "-c", "user.name=Test", "-c", "user.email=test@example.test",
        "commit", "-m", "original version")
    set_text(c, "unsaved version")
    staged = root / "staged.txt"
    staged.write_text("stage only")
    git(root, "add", "staged.txt")

    def snapshot():
        return (
            {str(path.relative_to(root)): (path.read_bytes(), path.stat().st_mode)
             for path in root.rglob("*") if path.is_file() and
             path.relative_to(root).parts[0] not in {".git", ".tsugumi"}},
            (root / ".git/index").read_bytes(),
            (root / ".git/HEAD").read_bytes(),
            git(root, "for-each-ref", "refs/heads"),
        )

    original = snapshot()
    # These roots are present in both versions. Each contributes two renames.
    roots = ("tsugumi.json", "tsugumi.lock", "layout.bin", "definitions",
             "notes", "externals", "skills", "templates")
    step_count = 2 * sum((root / name).exists() for name in roots) + 3
    for step in range(1, step_count + 1):
        daemon.stop()
        daemon.env.update(TSUGUMI_ENABLE_FAULTS="1", TSUGUMI_FAULT=f"checkout:{step}")
        daemon.start()
        c = daemon.connect()
        with pytest.raises((ConnectionError, TimeoutError)):
            c.call("VersionCheckoutRequest",
                   baseRev=c.call("ProjectInspectRequest").rev,
                   opId=new_id(), origin="test", commit=target)
        assert daemon.proc.wait(timeout=5) == 97, step
        assert (root / ".tsugumi/checkout.json").is_file(), step
        daemon.env.pop("TSUGUMI_ENABLE_FAULTS")
        daemon.env.pop("TSUGUMI_FAULT")
        daemon.restart()
        assert src_text(daemon.connect()) == "unsaved version", step
        assert snapshot() == original, step
        assert not (root / ".tsugumi/checkout.json").exists(), step

    reopened = daemon.connect()
    reopened.call("VersionCheckoutRequest",
                  baseRev=reopened.call("ProjectInspectRequest").rev,
                  opId=new_id(), origin="test", commit=target)
    assert src_text(reopened) == "target"


def test_runs_version_definitions_and_explicit_paths_only(tmp_path, core_factory):
    """Run commits and named versions exclude stray files and internal state."""
    root = tmp_path / "p"
    builtin_project(root)
    (root / ".gitignore").write_text("# State is not ignored here\n")
    (root / "result.csv").write_text("stray result")
    (root / "reference.csv").write_text("chosen input")
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    first = text(c.call("VersionInitRequest", opId=new_id(), origin="test").commit)

    def paths(commit):
        return set(git(root, "ls-tree", "-r", "--name-only", commit).splitlines())

    assert "result.csv" not in paths(first) and "reference.csv" not in paths(first)
    (root / "result.csv").write_text("another stray result")
    (root / ".tsugumi" / "sentinel").write_text("internal state")
    _, unchanged = run_and_wait(c, "start")
    assert unchanged.state == RunState.succeeded and text(unchanged.commit) == first
    assert not graph(c).dirty
    snap = c.call("ProjectInspectRequest")
    document = json.loads(text(snap.projectJson))
    document["trackedPaths"] = ["reference.csv"]
    c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="test",
           changes=[change("SetProject", json=json.dumps(document))])
    set_text(c, "changed definition")
    _, changed = run_and_wait(c, "start")
    second = text(changed.commit)
    assert changed.state == RunState.succeeded and second != first
    assert "reference.csv" in paths(second) and "result.csv" not in paths(second)
    assert "changed definition" in git(root, "show", second + ":definitions/src/params.nix")
    assert not any(".tsugumi" in p.split("/") for p in paths(second))
    (root / "reference.csv").write_text("changed chosen input")
    _, chosen = run_and_wait(c, "start")
    assert chosen.state == RunState.succeeded and text(chosen.commit) != second
    assert git(root, "show", text(chosen.commit) + ":reference.csv") == "changed chosen input"
    (root / "reference.csv").write_text("kept by hand")
    mark = _request_class("VersionMarkRequest")()
    mark.opId, mark.origin, mark.name = new_id(), "test", "chosen-input"
    c.wait_reply(c.send("VersionMarkRequest", mark))
    assert git(root, "show", "chosen-input:reference.csv") == "kept by hand"
    assert "result.csv" not in paths("chosen-input")
    assert not any(".tsugumi" in p.split("/") for p in paths("chosen-input"))
    assert (root / "result.csv").read_text() == "another stray result"
    assert (root / ".tsugumi" / "sentinel").read_text() == "internal state"



def test_a_project_with_a_note_runs_and_versions_the_note(tmp_path, core_factory):
    """Notes are project files: a run accepts them unchanged and commits them."""
    root = tmp_path / "p"
    builtin_project(root)
    (root / "notes").mkdir()
    (root / "notes" / "n1.md").write_text("A note without a final newline")
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    first = text(c.call("VersionInitRequest", opId=new_id(), origin="test").commit)
    assert git(root, "show", first + ":notes/n1.md") == "A note without a final newline"
    _, run = run_and_wait(c, "start")
    assert run.state == RunState.succeeded and text(run.commit) == first


def test_a_demo_history_becomes_its_versions(tmp_path, core_factory):
    """An imported project history retains its branches, named versions and run results."""
    root = tmp_path / "p"
    builtin_project(root, text="final")
    tried = params_nix({"text": "tried", "value": True})
    history = {
        "commits": [
            {"id": "plan", "message": "計画", "time": 1793700000, "files": {"definitions/src/params.nix": tried}},
            {"id": "try", "parents": ["plan"], "message": "別の文を試す", "time": 1793700600,
             "runs": [{"run_id": "11111111-1111-1111-1111-111111111111", "state": "failed", "ended": "2026-11-04T01:10:00Z", "seconds": 2.5}]},
            {"id": "final", "parents": ["plan"], "message": "採用", "time": 1793701200, "tag": "adopted",
             "runs": [{"run_id": "22222222-2222-2222-2222-222222222222", "state": "succeeded", "ended": "2026-11-04T01:20:00Z", "seconds": 1.2}]},
        ],
        "branches": {"main": "final", "try-other-text": "try"},
        "head": "main",
    }
    history["branches"] = [{"name": name, "commit": commit} for name, commit in history["branches"].items()]
    for commit in history["commits"]:
        if "files" in commit:
            commit["files"] = [{"path": path, "bytes": list(content.encode())}
                               for path, content in commit["files"].items()]
    (root / ".tsugumi-history").write_bytes(build_buffer("runtime_state", "Tsugumi.State.History", history))
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    c.call("VersionInitRequest", opId=new_id(), origin="test")
    g = graph(c)
    assert text(g.branch) == "main" and not g.dirty
    by = {text(x.summary): x for x in g.commits}
    assert set(by) == {"計画", "別の文を試す", "採用"}
    assert [text(t) for t in by["採用"].tags] == ["adopted"]
    assert [text(r.state) for r in by["別の文を試す"].runs] == ["failed"]
    assert not by["別の文を試す"].runs[0].summary  # the history file gives none
    assert src_text(c) == "final"



@pytest.mark.parametrize("language", ["en", "ja"])
def test_the_casa_demo_carries_its_trials(tmp_path, core_factory, language):
    """The Cas A project exposes its trials and can return to its original Drafts."""
    root = tmp_path / "casa"
    copy_demo(language, "casa", root)
    expected = json.loads((root / "tsugumi.json").read_text())
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    c.call("VersionInitRequest", opId=new_id(), origin="test")
    g = graph(c)
    head = text(g.head)
    assert any(text(run.state) == "failed" for entry in g.commits for run in entry.runs or [])
    # Going back to the plan gives the Drafts.
    plan = next(x for x in g.commits if not (x.parents or []))
    rev = c.call("ProjectInspectRequest").rev
    c.call("VersionCheckoutRequest", baseRev=rev, opId=new_id(), origin="test", commit=text(plan.id))
    snap = c.call("ProjectInspectRequest")
    drafts = {text(d.id) for d in snap.definitions if json.loads(text(d.cardJson)).get("type") == "draft"}
    assert drafts == {"load", "bands", "rgb"}
    for card in snap.resolution:
        if text(card.card.segments[0]) in drafts:
            assert not card.inputs and not card.outputs
    # The original experiments stay in Git even when their Card types have been removed.
    removed_input_trials = 0
    for entry in g.commits:
        commit = text(entry.id)
        if commit in (text(plan.id), head):
            continue
        load_card = json.loads(git(root, "show", f"{commit}:definitions/load/_card.json"))
        if load_card["type"] == "input":
            with pytest.raises(RpcFailure) as error:
                c.call("VersionCheckoutRequest", baseRev=snap.rev, opId=new_id(), origin="test", commit=commit)
            assert error.value.kind == "invalid_document"
            assert c.call("ProjectInspectRequest").rev == snap.rev
            removed_input_trials += 1
        else:
            c.call("VersionCheckoutRequest", baseRev=snap.rev, opId=new_id(), origin="test", commit=commit)
            snap = c.call("ProjectInspectRequest")
    assert removed_input_trials
    # Returning to the implemented tip restores its real ports and exact document.
    c.call("VersionCheckoutRequest", baseRev=snap.rev, opId=new_id(), origin="test", commit=head)
    restored = c.call("ProjectInspectRequest")
    assert json.loads(text(restored.projectJson)) == expected
    assert not graph(c).dirty



def summary_of(c, root, st) -> dict:
    """Read complete language-neutral facts from the public version graph."""
    runs = notes(c, root, text(st.commit), run_ids=(text(st.runId),))
    run = next(r for r in runs if text(r.runId) == text(st.runId))
    facts = run.facts
    assert facts is not None
    assert not run.summary
    return {"whole": facts.whole, "card": text(facts.card), "propagate": facts.propagate,
            "saved_inputs": facts.savedInputs,
            "ran": [{"card": text(x.card), "count": x.count} for x in facts.ran or []],
            "failures": [{"card": text(x.card), "kind": text(x.kind)} for x in facts.failures or []],
            "never": [text(x) for x in facts.never or []]}


def run_facts(*, whole=False, card="", propagate=True, saved=False, ran=(), failed=(), never=()):
    return {"whole": whole, "card": card, "propagate": propagate, "saved_inputs": saved,
            "ran": [{"card": c, "count": n} for c, n in ran],
            "failures": [{"card": c, "kind": k} for c, k in failed], "never": list(never)}





def test_a_run_record_is_independent_of_the_connection(tmp_path, core_factory):
    """Every connection reads the same complete, neutral facts."""
    root = tmp_path / "p"
    independent = [f"more{i}" for i in range(7)]
    write_nix_project(root,
          defs={"trigger": {"type": "trigger"},
                "source": {"type": "code", "params": {"text": "first", "value": True},
                           "_outputs": MSG_FBS, "_nix": MSG_SOURCE_NIX},
                "merge": {"type": "merge", "inputs": [{"name": "a"}, {"name": "b"}], "maxRuns": 4},
                "sink": dict(SINK, maxRuns=4)},
          cards=[card("start", "trigger"), card("src", "source"), card("both", "merge"), card("sink", "sink")] + [card(name, "source") for name in independent],
          conns=[conn("src", "msg", "both", "a"), conn("src", "msg", "both", "b"), conn("both", "out", "sink", "in")])
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    c.call("VersionInitRequest", opId=new_id(), origin="test")
    created = graph(c).commits[0]
    assert not created.message and text(created.facts.kind) == "create" and not created.facts.card
    _, st = run_and_wait(c, "both")
    assert summary_of(c, root, st) == run_facts(card="both", failed=[("both", "input_missing")], never=["sink"])
    set_text(c, "second")
    ran = [("src", 1), ("both", 2), ("sink", 2)]
    ran.extend((name, 1) for name in independent)
    expected = run_facts(whole=True, card="start", ran=ran)
    first_run, st = run_and_wait(c, "start")
    assert summary_of(c, root, st) == expected
    committed = next(entry for entry in graph(c).commits if text(entry.id) == text(st.commit))
    assert not committed.message and text(committed.facts.kind) == "run" and text(committed.facts.card) == "start"
    _, st = run_and_wait(c, "both")
    assert summary_of(c, root, st) == run_facts(card="both", saved=True, ran=[("both",1),("sink",1)])
    _, st = run_and_wait(c, "sink", propagate=False)
    assert summary_of(c, root, st) == run_facts(card="sink", propagate=False, saved=True, ran=[("sink",1)])
    # Another connection, in Japanese, on the same project.
    j = daemon.connect()
    set_text(j, "third")
    second_run, st = run_and_wait(j, "start")
    assert summary_of(j, root, st) == expected
    committed = next(entry for entry in graph(j).commits if text(entry.id) == text(st.commit))
    assert not committed.message and text(committed.facts.kind) == "run" and text(committed.facts.card) == "start"

    daemon.restart()
    restored = daemon.connect()
    for run_id, expected_value in ((first_run, "second"), (second_run, "third")):
        row = restored.call("RunRecordRequest", runId=run_id, card=path("src"))
        assert text(row.run.status.runId) == run_id
        assert {text(p.name): json.loads(text(p.value)) for p in row.record.params.entries}["text"] == expected_value


def test_a_run_note_names_a_card_that_ran_and_failed(tmp_path, core_factory):
    """A run summary identifies the Card that exceeded its execution limit."""
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("feed", "source"), card("both", "merge"), card("sink", "sink")],
                    extra_conns=[conn("feed", "msg", "both", "a"), conn("feed", "msg", "both", "b"), conn("both", "out", "sink", "in")])
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    c.call("VersionInitRequest", opId=new_id(), origin="test")
    _, st = run_and_wait(c, "feed")
    assert st.state == RunState.failed
    summary = summary_of(c, root, st)
    assert summary == run_facts(card="feed", ran=[("feed",1),("both",2),("sink",1)], failed=[("both","execution_limit")]), summary
    assert any(text(run.state) == "failed" for run in notes(c, root, text(st.commit), 1))



def test_a_versions_canvas_says_what_each_card_is(tmp_path, core_factory):
    """Each version retains its Card kinds, port counts and canvas layout."""
    def commit_all(project):  # these Cards are not wired to run: a version of them is committed by hand
        git(project, "add", "-A")
        git(project, "-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "--allow-empty", "-qm", "next")
        return git(project, "rev-parse", "HEAD").strip()

    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("wide", "merge"), card("m", "merge")],
                    extra_conns=[conn("src", "msg", "m", "a")])
    wide = root / "definitions" / "wide" / "_card.json"
    wide.write_text(json.dumps(card_document({"type": "merge", "inputs": [{"name": "a"}, {"name": "b"}, {"name": "c"}]})))
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    first = text(c.call("VersionInitRequest", opId=new_id(), origin="test").commit)
    canvas = c.call("VersionCanvasRequest", commit=first)
    cards = {text(k.id): k for k in canvas.cards}
    assert {k: text(v.kind) for k, v in cards.items()} == {
        "start": "trigger", "src": "code", "out": "collect", "wide": "merge", "m": "merge"}
    assert (cards["wide"].fan, cards["m"].fan, cards["out"].fan) == (3, 2, 0)
    write_layout(root, {"cards": {"out": {"x": 3, "y": 7, "collapsed": True}}})
    assert bytes(c.call("VersionCanvasRequest", commit=commit_all(root)).layout) == (root / "layout.bin").read_bytes()
    # An older version keeps what its Cards were.
    wide.write_text(json.dumps(card_document({"type": "merge", "inputs": [{"name": "a"}]})))
    now = {text(k.id): k for k in c.call("VersionCanvasRequest", commit=commit_all(root)).cards}
    assert (text(now["wide"].kind), now["wide"].fan) == ("merge", 1)
    then = {text(k.id): k for k in c.call("VersionCanvasRequest", commit=first).cards}
    assert (text(then["wide"].kind), then["wide"].fan) == ("merge", 3)



def test_a_version_has_its_canvas_and_neutral_change_record(tmp_path, core_factory):
    """Each version retains its canvas and value-free change kinds."""
    root = tmp_path / "p"
    builtin_project(root, text="first",
                    extra_cards=[card("later", "source")])
    (root / "definitions" / "src" / "data").mkdir()
    (root / "definitions" / "src" / "data" / "secret.csv").write_text("patient,1\n")
    (root / "definitions" / "src" / "src").mkdir()
    (root / "definitions" / "src" / "src" / "values.csv").write_text("patient,2\n")
    (root / "definitions" / "src" / "src" / "main.py").write_text("print('code one')\n")
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    first = text(c.call("VersionInitRequest", opId=new_id(), origin="test").commit)
    # No saved positions: columns by depth, and only real wires.
    canvas = c.call("VersionCanvasRequest", commit=first)
    at = {text(k.id): None for k in canvas.cards}
    assert set(at) == {"start", "src", "out", "later"}
    assert [(text(w.from_), text(w.to)) for w in canvas.wires] == [("src", "out")]
    # A later version: its diff is what it changed, and the older version
    # keeps its own Cards.
    set_text(c, "second")
    (root / "definitions" / "src" / "data" / "secret.csv").write_text("patient,3\n")
    write_layout(root, {"cards": {k: {"x": i * 100, "y": 7} for i, k in enumerate(at)}})
    c.call("ProjectReloadRequest", opId=new_id(), origin="test", baseRev=c.call("ProjectInspectRequest").rev)
    _, st = run_and_wait(c, "start")
    second = text(st.commit)
    placed = positions(c.call("VersionCanvasRequest", commit=second).layout)
    assert placed["src"]["y"] == 7 and placed["out"]["x"] == 200
    # Core records neutral kinds without parameter or source values.
    factual = c.call("VersionNoteRequest", commit=second)
    facts = factual.changes
    record = {"added": [text(x) for x in facts.added or []],
              "removed": [text(x) for x in facts.removed or []],
              "changed": [{"card": text(x.card), "kinds": [text(k) for k in x.kinds or []]} for x in facts.changed or []],
              "workflow": facts.workflow}
    assert record == {"added": [], "removed": [], "changed": [{"card": "src", "kinds": ["parameters"]}], "workflow": False}
    from Tsugumi.RPC.VersionChanges import VersionChanges
    raw_note = subprocess.check_output(["git", "-C", str(root), "notes", "--ref=tsugumi-record", "show", second])
    cached = VersionChanges.GetRootAs(raw_note)
    assert cached.Changed(0).Card() == b"src"
    assert cached.Changed(0).Kinds(0) == b"parameters"
    assert all(text(run.state) == "succeeded" for run in notes(c, root, second, 1))
    # A commit that is not there is refused, and nothing is kept.
    try:
        c.call("VersionCanvasRequest", commit="0" * 40)
        raise AssertionError("no such version")
    except RpcFailure as e:
        assert e.kind == "not_found"
