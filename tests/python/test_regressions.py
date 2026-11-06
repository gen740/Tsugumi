"""Reproductions of reported core defects; each must stay fixed.

1. A run whose invocation records could not be saved must not succeed.
2. File values pass through fork/branch (forwarding an input is allowed).
3. Saved inputs for a single-Card run keep File contents across CacheClear and restarts.
4. A run stalled by a full downstream queue ends instead of hanging
   (needs a code Card feedback loop: test_cards_container.py).
5. A Logger Card shows numbers exactly (it used to keep 12 decimals, so 1e-20 was 0.0).
6. A Card under a name another Card had does not get that Card's display.
"""

from __future__ import annotations

import json
import time
from pathlib import Path

from conftest import Daemon
from projects import FILE_FBS, builtin_project, card, conn, put, run_and_wait, write
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload

def file_fork_project(root: Path) -> None:
    write(root,
          defs={"trigger": {"template": "trigger"},
                "doc": {"template": "input", "params": [{"name": "value", "type": "string", "data": "definitions/doc/data/report.txt"}],
                        "_outputs": FILE_FBS},
                "fork": {"template": "fork"},
                "log": {"template": "logger"}},
          cards=[card("src", "doc"), card("split", "fork"), card("a", "log"), card("b", "log")],
          conns=[conn("src", "doc", "split", "in"), conn("split", "out1", "a", "in"), conn("split", "out2", "b", "in")],
          files={"definitions/doc/data/report.txt": "quarterly numbers\n"})


def test_history_write_failure_fails_the_run(tmp_path):
    root = tmp_path / "p"
    file_fork_project(root)
    d = Daemon(root, env={"TSUGUMI_ENABLE_FAULTS": "1", "TSUGUMI_FAULT": "history_invocation"}).start()
    try:
        c = d.client()
        c.hello()
        _, st = run_and_wait(c, "src")
        assert st.state == RunState.failed
        assert st.failure.kind == 25  # persistence_failed
        for k in ("src", "split", "a", "b"):
            try:
                page = c.call("RunInvocationsRequest", runId=text(st.runId), card=path(k))
            except RpcFailure as e:
                assert e.kind == "not_found"  # nothing recorded for it
                continue
            assert all(r.state != 6 for r in page.records)  # nothing unrecorded claims success
    finally:
        d.cleanup()


def test_file_passes_through_fork(tmp_path):
    root = tmp_path / "p"
    file_fork_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, st = run_and_wait(c, "src")
        assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
        for k in ("a", "b"):
            log = c.call("LogRangeRequest", runId=run_id, card=path(k), invocation=1, stream=1)
            assert "report.txt" in text(log.text)
    finally:
        d.cleanup()


def test_saved_file_inputs_survive_cache_clear_and_restart(tmp_path):
    root = tmp_path / "p"
    file_fork_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        _, st = run_and_wait(c, "src")
        assert st.state == RunState.succeeded
        # The produced file lives in the run's work area, which CacheClear removes.
        c.call("ProjectCacheClearRequest", opId=new_id())
        _, st2 = run_and_wait(c, "split", propagate=False)
        assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
        d.stop()
        d.start()
        c = d.client()
        c.hello()
        _, st3 = run_and_wait(c, "split", propagate=False)
        assert st3.state == RunState.succeeded, st3.failure and text(st3.failure.diagnostic)
        # The logger downstream also kept its own saved input.
        _, st4 = run_and_wait(c, "a", propagate=False)
        assert st4.state == RunState.succeeded
    finally:
        d.cleanup()


def test_saved_inputs_missing_port_fails(tmp_path):
    root = tmp_path / "p"
    file_fork_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        _, st = run_and_wait(c, "split", propagate=False)
        assert st.state == RunState.failed
        assert "saved" in text(st.failure.diagnostic)
    finally:
        d.cleanup()


def test_rerun_from_saved_file_inputs_with_much_shorter_paths(tmp_path):
    """Saved copies of a File input (per invocation and per port) can have a
    shorter path than the original; rewriting the path must shrink the string
    without corrupting the buffer (flatbuffers' SetString cannot shrink)."""
    # The producer's work path contains its Card id; a long id makes the saved copies much shorter.
    src = "source-" + "x" * 80
    root = tmp_path / "p"
    write(root,
          defs={"doc": {"template": "input", "params": [{"name": "value", "type": "string", "data": "definitions/doc/report.txt"}],
                        "_outputs": FILE_FBS},
                "join": {"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}]},
                "log": {"template": "logger"}},
          cards=[card(src, "doc"), card("join", "join"), card("out", "log")],
          conns=[conn(src, "doc", "join", "a"), conn("join", "out", "out", "in")],
          files={"definitions/doc/report.txt": "quarterly numbers\n"})
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        _, st = run_and_wait(c, src)
        assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
        # The merge reruns from its saved value; the logger shows the path it received.
        run_id, st2 = run_and_wait(c, "join")
        assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
        log = c.call("LogRangeRequest", runId=run_id, card=path("out"), invocation=1, stream=1)
        received = json.loads(text(log.text))["value"]["value"]
        assert received.endswith("/a/report.txt") and Path(received).read_text() == "quarterly numbers\n"
    finally:
        d.cleanup()


def test_cancel_at_any_moment_of_a_run_keeps_tsugumid_well(tmp_path):
    """Cancelling as a run starts, runs or ends: callbacks of a run that has
    ended still find it (they hold it), so tsugumid answers throughout, and the
    run always ends (a cancel just after its final status was taken once wrote
    "cancelling" over it, about 1 attempt in 100: hence the many attempts)."""
    from projects import builtin_project, start_run, wait_run
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        for i in range(150):
            rid = start_run(c, "start")
            if i % 3:
                time.sleep(0.0005 * (i % 30))
            try:
                c.call("RunCancelRequest", runId=rid)
            except RpcFailure:
                pass  # it had already ended
            wait_run(c, rid, timeout=60)
        assert d.proc.poll() is None
        c.call("ProjectInspectRequest")
    finally:
        d.cleanup()


def test_a_logger_shows_numbers_exactly(tmp_path):
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
                "log": {"template": "logger"},
                "notes": {"template": "logger", "markdown": True}},
          cards=[card("src", "reading"), card("split", "fork"), card("plain", "log"), card("md", "notes")],
          conns=[conn("src", "reading", "split", "in"), conn("split", "out1", "plain", "in"), conn("split", "out2", "md", "in")])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, st = run_and_wait(c, "src")
        assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
        line = '{"rate": 0.042169650342858,"tiny": 1e-20}'
        shown = lambda card_id: c.call("RunRecordRequest", runId=run_id, card=path(card_id)).record.display.value
        assert text(shown("plain").text) == line
        assert text(c.call("LogRangeRequest", runId=run_id, card=path("plain"), invocation=1, stream=1).text) == line + "\n"
        assert shown("md").markdown and text(shown("md").text) == "```json\n" + line + "\n```"
    finally:
        d.cleanup()


class FreedNames:
    """A logger that has shown something, and the saves around it."""

    def __init__(self, root: Path):
        builtin_project(root, text="the old result")
        self.d = Daemon(root).start()
        self.c = self.d.client()
        self.c.hello()
        self.sub = text(self.c.open().subscriptionId)
        _, st = run_and_wait(self.c, "src")
        assert st.state == RunState.succeeded

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


def test_a_renamed_card_keeps_its_display_and_leaves_none_behind(tmp_path):
    t = FreedNames(tmp_path / "p")
    try:
        assert "the old result" in t.shown("out")[0]
        t.save(["start", "src", "kept"], change("RenameDefinition", id="out", to="kept"))
        moved = t.shown("kept")
        assert moved and "the old result" in moved[0] and moved[1] == "kept"
        assert t.shown("out") is None
        # The old name taken by another Card: it starts without a display.
        t.save(["start", "src", "kept", "out"], change("PutDefinition", id="out", cardJson='{"template": "logger"}'))
        assert t.shown("out") is None
    finally:
        t.d.stop()


def test_a_card_under_a_deleted_cards_name_does_not_get_its_display(tmp_path):
    t = FreedNames(tmp_path / "p")
    try:
        assert "the old result" in t.shown("out")[0]
        t.save(["start", "src"], change("RemoveDefinition", id="out"))
        # A Card made otherwise: nothing, from the last run's history either, and
        # asking again does not bring it back.
        t.save(["start", "src", "out"], change("PutDefinition", id="out", cardJson='{"template": "logger", "markdown": true}'))
        assert t.shown("out") is None
        assert t.shown("out") is None
    finally:
        t.d.stop()


def test_a_card_that_comes_to_a_deleted_cards_name_or_leaves_it_gets_nothing(tmp_path):
    t = FreedNames(tmp_path / "p")
    try:
        assert "the old result" in t.shown("out")[0]
        t.save(["start", "src"], change("RemoveDefinition", id="out"))
        # A new Draft takes the name, and is named by the user: the old display stays behind.
        draft = '{"template": "draft"}'
        t.save(["start", "src", "out"], change("PutDefinition", id="out", cardJson=draft))
        t.save(["start", "src", "named"], change("RenameDefinition", id="out", to="named"))
        t.save(["start", "src", "named"], change("PutDefinition", id="named", cardJson='{"template": "logger", "markdown": true}'))
        assert t.shown("named") is None
        # Another Card renamed onto the old name: nothing either.
        t.save(["start", "src", "named", "other"], change("PutDefinition", id="other", cardJson=draft))
        t.save(["start", "src", "named", "out"], change("RenameDefinition", id="other", to="out"))
        t.save(["start", "src", "named", "out"], change("PutDefinition", id="out", cardJson='{"template": "logger", "markdown": true}'))
        assert t.shown("out") is None
    finally:
        t.d.stop()


def test_a_card_put_back_as_it_was_has_its_display_again(tmp_path):
    t = FreedNames(tmp_path / "p")
    try:
        assert "the old result" in t.shown("out")[0]
        t.save(["start", "src"], change("RemoveDefinition", id="out"))
        # Written with other spacing than the file it came from: the same Card all the same.
        t.save(["start", "src", "out"], change("PutDefinition", id="out", cardJson='{ "template":"logger" }'))
        got = t.shown("out")
        assert got and "the old result" in got[0]
    finally:
        t.d.stop()
