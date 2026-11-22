"""Public RPC regressions for saved inputs and cancellation."""

from __future__ import annotations

from pathlib import Path

from harness.observations import decode_scalar
from harness.values import recorded_input_json
from projects import FILE_FBS, SINK, card, conn, run_and_wait, write
from tsugumi_client import RpcFailure, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.RunRecordCategory import RunRecordCategory

def file_fanout_project(root: Path) -> None:
    write(root,
          defs={"trigger": {"type": "trigger"},
                "doc": {"type": "input", "params": [{"name": "value", "type": "string", "data": "data/report.txt"}],
                        "_outputs": FILE_FBS, "retainArtifact": True},
                "sink": dict(SINK, retainArtifact=True)},
          cards=[card("src", "doc"), card("a", "sink"), card("b", "sink")],
          conns=[conn("src", "doc", "a", "in"), conn("src", "doc", "b", "in")],
          files={"definitions/doc/data/report.txt": "quarterly numbers\n"})


def test_history_write_failure_fails_the_run(tmp_path, core_factory):
    """A run cannot succeed when its invocation record could not be saved."""
    root = tmp_path / "p"
    file_fanout_project(root)
    d = core_factory(project_root=root, fault="invocation_record")
    c = d.connect()
    _, st = run_and_wait(c, "src")
    assert st.state == RunState.failed
    assert st.failure.kind == FailureKind.persistence_failed
    for k in ("src", "a", "b"):
        try:
            page = c.call("RunInvocationsRequest", runId=text(st.runId), card=path(k))
        except RpcFailure as e:
            assert e.kind == "not_found"  # nothing recorded for it
            continue
        assert all(r.state != CardState.succeeded for r in page.records)  # nothing unrecorded claims success


def test_saved_file_inputs_survive_cache_clear_and_restart(tmp_path, core_factory):
    """Partial runs reuse saved file inputs after cache clearing and restart."""
    root = tmp_path / "p"
    file_fanout_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    _, missing = run_and_wait(c, "b", propagate=False)
    assert missing.failure.kind == FailureKind.input_missing
    first, st = run_and_wait(c, "src")
    assert st.state == RunState.succeeded
    for branch in ("a", "b"):
        record = c.call("RunRecordRequest", runId=first, card=path(branch), invocation=1).record
        value = next(value for value in record.inputs if text(value.name) == "in")
        assert Path(text(value.filePath)).read_text() == "quarterly numbers\n"

    c.call("RunCategoryDeleteRequest", runId=first, category=RunRecordCategory.Files, opId=new_id())
    c.call("ProjectCacheClearRequest", opId=new_id())
    _, st2 = run_and_wait(c, "b", propagate=False)
    assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
    c.call("RunScratchDeleteRequest", opId=new_id(), paths=[f"runs/{first}"])
    d.stop()
    d.start()
    c = d.connect()
    _, st3 = run_and_wait(c, "b", propagate=False)
    assert st3.state == RunState.succeeded, st3.failure and text(st3.failure.diagnostic)
    # The downstream Card also kept its own saved input.
    _, st4 = run_and_wait(c, "a", propagate=False)
    assert st4.state == RunState.succeeded


def test_rerun_from_saved_file_inputs_with_much_shorter_paths(tmp_path, core_factory):
    """A partial merge run preserves a saved file value from a long source name."""
    # A long producer name exercises a saved File value whose path must change.
    src = "source-" + "x" * 80
    root = tmp_path / "p"
    write(root,
          defs={"doc": {"type": "input", "params": [{"name": "value", "type": "string", "data": "report.txt"}],
                        "_outputs": FILE_FBS},
                "join": {"type": "merge", "inputs": [{"name": "a"}, {"name": "b"}]},
                "sink": dict(SINK, recordIO=True)},
          cards=[card(src, "doc"), card("join", "join"), card("out", "sink")],
          conns=[conn(src, "doc", "join", "a"), conn("join", "out", "out", "in")],
          files={"definitions/doc/report.txt": "quarterly numbers\n"})
    d = core_factory(project_root=root)
    c = d.connect()
    _, st = run_and_wait(c, src)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    # The merge reruns from its saved value; the sink records the path it received.
    run_id, st2 = run_and_wait(c, "join")
    assert st2.state == RunState.succeeded, st2.failure and text(st2.failure.diagnostic)
    received = recorded_input_json(c, tmp_path / "decode", run_id, "out")
    assert received["value"]["value"].endswith("/report.txt")
    assert received["value_type"] == "a"




def test_direct_fanout_preserves_precise_numbers(tmp_path, core_factory):
    """Both branches retain small and precise typed floating-point values."""
    root = tmp_path / "p"
    write(root,
          defs={"trigger": {"type": "trigger"},
                "reading": {"type": "input",
                            "params": [{"name": "rate", "type": "double", "data": 0.042169650342858},
                                       {"name": "tiny", "type": "double", "data": 1e-20}],
                            "_outputs": """
namespace nums;
table Reading { rate:double; tiny:double; }
table Outputs { reading:Reading (required); }
root_type Outputs;
"""},
                "sink": dict(SINK, recordIO=True)},
          cards=[card("src", "reading"), card("a", "sink"), card("b", "sink")],
          conns=[conn("src", "reading", "a", "in"), conn("src", "reading", "b", "in")])
    d = core_factory(project_root=root)
    c = d.connect()
    run_id, st = run_and_wait(c, "src")
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    for card_id in ("a", "b"):
        record = c.call("RunRecordRequest", runId=run_id, card=path(card_id)).record
        payload = bytes(record.inputs[0].inlineBytes)
        assert decode_scalar(payload, "double", field=0) == 0.042169650342858
        assert decode_scalar(payload, "double", field=1) == 1e-20
