"""Public RPC regressions for saved inputs, cancellation and display identity."""

from __future__ import annotations

import json
import time
from pathlib import Path

from harness.fixtures import add_worker
from harness.observations import decode_scalar
from harness.scenario import Project
from harness.values import recorded_input_json
from projects import FILE_FBS, SINK, builtin_project, card, conn, run_and_wait, start_run, wait_run, write
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload

def file_fork_project(root: Path) -> None:
    write(root,
          defs={"trigger": {"template": "trigger"},
                "doc": {"template": "input", "params": [{"name": "value", "type": "string", "data": "data/report.txt"}],
                        "_outputs": FILE_FBS},
                "fork": {"template": "fork"},
                "sink": dict(SINK, recordIO=True)},
          cards=[card("src", "doc"), card("split", "fork"), card("a", "sink"), card("b", "sink")],
          conns=[conn("src", "doc", "split", "in"), conn("split", "out1", "a", "in"), conn("split", "out2", "b", "in")],
          files={"definitions/doc/data/report.txt": "quarterly numbers\n"})


def test_history_write_failure_fails_the_run(tmp_path, core_factory):
    """A run cannot succeed when its invocation record could not be saved."""
    root = tmp_path / "p"
    file_fork_project(root)
    d = core_factory(project_root=root, fault="invocation_record")
    c = d.connect()
    _, st = run_and_wait(c, "src", timeout=10)
    assert st.state == RunState.failed
    assert st.failure.kind == FailureKind.persistence_failed
    for k in ("src", "split", "a", "b"):
        try:
            page = c.call("RunInvocationsRequest", runId=text(st.runId), card=path(k))
        except RpcFailure as e:
            assert e.kind == "not_found"  # nothing recorded for it
            continue
        assert all(r.state != CardState.succeeded for r in page.records)  # nothing unrecorded claims success


def test_file_passes_through_fork(tmp_path, core_factory):
    """Both downstream cards receive a file forwarded through a fork."""
    root = tmp_path / "p"
    file_fork_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    run_id, st = run_and_wait(c, "src", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    for k in ("a", "b"):
        value = recorded_input_json(c, tmp_path / "decode", run_id, k)
        assert value["value"].endswith("/report.txt")


def test_saved_file_inputs_survive_cache_clear_and_restart(tmp_path, core_factory):
    """Partial runs reuse saved file inputs after cache clearing and restart."""
    root = tmp_path / "p"
    file_fork_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    _, st = run_and_wait(c, "src", timeout=10)
    assert st.state == RunState.succeeded
    # The next partial run removes earlier scratch; saved inputs have their own copy.
    c.call("ProjectCacheClearRequest", opId=new_id())
    _, st2 = run_and_wait(c, "split", propagate=False, timeout=10)
    assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
    d.stop()
    d.start()
    c = d.connect()
    _, st3 = run_and_wait(c, "split", propagate=False, timeout=10)
    assert st3.state == RunState.succeeded, st3.failure and text(st3.failure.diagnostic)
    # The downstream Card also kept its own saved input.
    _, st4 = run_and_wait(c, "a", propagate=False, timeout=10)
    assert st4.state == RunState.succeeded


def test_saved_inputs_missing_port_fails(tmp_path, core_factory):
    """A partial run reports its missing saved file input."""
    root = tmp_path / "p"
    file_fork_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    _, st = run_and_wait(c, "split", propagate=False, timeout=10)
    assert st.state == RunState.failed
    assert "saved" in text(st.failure.diagnostic)


def test_rerun_from_saved_file_inputs_with_much_shorter_paths(tmp_path, core_factory):
    """A partial merge run preserves a saved file value from a long source name."""
    # A long producer name exercises a saved File value whose path must change.
    src = "source-" + "x" * 80
    root = tmp_path / "p"
    write(root,
          defs={"doc": {"template": "input", "params": [{"name": "value", "type": "string", "data": "report.txt"}],
                        "_outputs": FILE_FBS},
                "join": {"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}]},
                "sink": dict(SINK, recordIO=True)},
          cards=[card(src, "doc"), card("join", "join"), card("out", "sink")],
          conns=[conn(src, "doc", "join", "a"), conn("join", "out", "out", "in")],
          files={"definitions/doc/report.txt": "quarterly numbers\n"})
    d = core_factory(project_root=root)
    c = d.connect()
    _, st = run_and_wait(c, src, timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    # The merge reruns from its saved value; the sink records the path it received.
    run_id, st2 = run_and_wait(c, "join", timeout=10)
    assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
    received = recorded_input_json(c, tmp_path / "decode", run_id, "out")
    assert received["value"]["value"].endswith("/report.txt")
    assert received["value_type"] == "a"


def test_cancel_at_any_moment_of_a_run_keeps_tsugumid_well(tmp_path, core_factory):
    """Cancellation near run completion always reaches a terminal state."""
    root = tmp_path / "p"
    builtin_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    for i in range(150):
        rid = start_run(c, "start")
        if i % 3:
            time.sleep(0.0005 * (i % 30))
        try:
            c.call("RunCancelRequest", runId=rid)
        except RpcFailure:
            pass  # it had already ended
        wait_run(c, rid, timeout=10)
    c.call("ProjectInspectRequest")


def test_fork_preserves_precise_numbers(tmp_path, core_factory):
    """Both branches retain small and precise typed floating-point values."""
    root = tmp_path / "p"
    write(root,
          defs={"trigger": {"template": "trigger"},
                "reading": {"template": "input",
                            "params": [{"name": "rate", "type": "double", "data": 0.042169650342858},
                                       {"name": "tiny", "type": "double", "data": 1e-20}],
                            "_outputs": """
namespace nums;
table Reading { rate:double; tiny:double; }
table Outputs { reading:Reading (required); }
root_type Outputs;
"""},
                "fork": {"template": "fork"},
                "sink": dict(SINK, recordIO=True)},
          cards=[card("src", "reading"), card("split", "fork"), card("a", "sink"), card("b", "sink")],
          conns=[conn("src", "reading", "split", "in"), conn("split", "out1", "a", "in"),
                 conn("split", "out2", "b", "in")])
    d = core_factory(project_root=root)
    c = d.connect()
    run_id, st = run_and_wait(c, "src", timeout=10)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    for card_id in ("a", "b"):
        record = c.call("RunRecordRequest", runId=run_id, card=path(card_id)).record
        payload = bytes(record.inputs[0].inlineBytes)
        assert decode_scalar(payload, "double", field=0) == 0.042169650342858
        assert decode_scalar(payload, "double", field=1) == 1e-20


class FreedNames:
    """A Card that has shown something, and the saves around it."""

    def __init__(self, root: Path, core_factory):
        write(root, {}, [], [])
        daemon = core_factory(project_root=root, mocked_builds=True, mocked_worker=True)
        self.c = daemon.connect()
        add_worker(Project(root), "out", root.parent / "display-worker",
                   [{"op": "display_text", "text": "the old result"}]).save(self.c)
        opened = self.c.open()
        self.sub = text(opened.subscriptionId)
        definition = next(d for d in opened.snapshot.definitions if text(d.id) == "out")
        self.definition = json.loads(text(definition.cardJson))
        self.sources = {}
        for source in opened.snapshot.sources:
            filename = text(source.path)
            if filename.startswith("definitions/out/") and filename != "definitions/out/_card.json":
                self.sources[filename] = bytes(self.c.call("ProjectFileReadRequest", path=filename).bytes)
        _, st = run_and_wait(self.c, "out", timeout=10)
        assert st.state == RunState.succeeded

    def restore(self):
        changes = [change("PutDefinition", id="out", cardJson=json.dumps(self.definition, separators=(",", ":")))]
        changes.extend(change("PutSource", path=filename, bytes=source) for filename, source in self.sources.items())
        self.save(["out"], *changes)

    def shown(self, card: str):
        """What the core sends for a Card that becomes visible: its text and its Card."""
        self.c.notifications.clear()
        self.c.call("ViewSetVisibleRequest", subscriptionId=self.sub, cards=[path(card)])
        try:
            n = self.c.wait_notification(lambda n: n.valueType == NotificationPayload.DisplayUpdated, timeout=1.0)
        except TimeoutError:
            return None
        return text(n.value.display.value.text), "/".join(text(s) for s in n.value.display.card.segments)

    def save(self, cards: list, *changes):
        snap = self.c.call("ProjectInspectRequest")
        project = json.loads(text(snap.projectJson))
        project["canvas"]["cards"] = [{"id": i} for i in cards]
        project["canvas"]["connections"] = []
        self.c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
                    changes=[change("SetProject", json=json.dumps(project)), *changes])


def test_a_renamed_card_keeps_its_display_and_leaves_none_behind(tmp_path, core_factory):
    """Renaming a card moves its display without leaving one at the old name."""
    t = FreedNames(tmp_path / "p", core_factory)
    assert "the old result" in t.shown("out")[0]
    t.save(["kept"], change("RenameDefinition", id="out", to="kept"))
    moved = t.shown("kept")
    assert moved and "the old result" in moved[0] and moved[1] == "kept"
    assert t.shown("out") is None
    # The old name taken by another Card: it starts without a display.
    t.save(["kept", "out"], change("PutDefinition", id="out", cardJson=json.dumps(SINK)))
    assert t.shown("out") is None


def test_a_card_under_a_deleted_cards_name_does_not_get_its_display(tmp_path, core_factory):
    """A different card reusing a deleted name does not inherit the old display."""
    t = FreedNames(tmp_path / "p", core_factory)
    assert "the old result" in t.shown("out")[0]
    t.save([], change("RemoveDefinition", id="out"))
    # A Card made otherwise: nothing, from the last run's history either, and
    # asking again does not bring it back.
    t.save(["out"], change("PutDefinition", id="out", cardJson=json.dumps(SINK)))
    assert t.shown("out") is None
    assert t.shown("out") is None


def test_a_card_that_comes_to_a_deleted_cards_name_or_leaves_it_gets_nothing(tmp_path, core_factory):
    """Renaming a replacement card cannot revive another card's deleted display."""
    t = FreedNames(tmp_path / "p", core_factory)
    assert "the old result" in t.shown("out")[0]
    t.save([], change("RemoveDefinition", id="out"))
    # A new Draft takes the name, and is named by the user: the old display stays behind.
    draft = '{"template": "draft"}'
    t.save(["out"], change("PutDefinition", id="out", cardJson=draft))
    t.save(["named"], change("RenameDefinition", id="out", to="named"))
    t.save(["named"], change("PutDefinition", id="named", cardJson=json.dumps(SINK)))
    assert t.shown("named") is None
    # Another Card renamed onto the old name: nothing either.
    t.save(["named", "other"], change("PutDefinition", id="other", cardJson=draft))
    t.save(["named", "out"], change("RenameDefinition", id="other", to="out"))
    t.save(["named", "out"], change("PutDefinition", id="out", cardJson=json.dumps(SINK)))
    assert t.shown("out") is None


def test_a_card_put_back_as_it_was_has_its_display_again(tmp_path, core_factory):
    """Restoring an equivalent card restores its matching recorded display."""
    t = FreedNames(tmp_path / "p", core_factory)
    assert "the old result" in t.shown("out")[0]
    t.save([], change("RemoveDefinition", id="out"))
    # Written with other spacing than the file it came from: the same Card all the same.
    t.restore()
    got = t.shown("out")
    assert got and "the old result" in got[0]
