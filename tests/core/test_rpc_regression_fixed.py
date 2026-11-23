"""Public RPC regressions for run persistence and typed values."""

from __future__ import annotations

from harness.observations import decode_scalar
from projects import SINK, card, conn, run_and_wait, write
from tsugumi_client import RpcFailure, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState

def test_history_write_failure_fails_the_run(tmp_path, core_factory):
    """A run cannot succeed when its invocation record could not be saved."""
    root = tmp_path / "p"
    write(root, defs={"start": {"type": "trigger"}}, cards=[card("src", "start")], conns=[])
    d = core_factory(project_root=root, fault="invocation_record")
    c = d.connect()
    _, st = run_and_wait(c, "src")
    assert st.state == RunState.failed
    assert st.failure.kind == FailureKind.persistence_failed
    try:
        page = c.call("RunInvocationsRequest", runId=text(st.runId), card=path("src"))
    except RpcFailure as e:
        assert e.kind == "not_found"
    else:
        assert all(r.state != CardState.succeeded for r in page.records)


def test_direct_fanout_preserves_precise_numbers(tmp_path, core_factory):
    """Both branches retain small and precise typed floating-point values."""
    root = tmp_path / "p"
    write(root,
          defs={"trigger": {"type": "trigger"},
                "reading": {"type": "input",
                            "params": {"rate": 0.042169650342858, "tiny": 1e-20}, "_params": '{ rate = 0.0; tiny = 0.0; }',
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
