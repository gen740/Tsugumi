"""Check that a disagreement reaches the Collatz Card’s failure path."""
import runpy
import sys
from types import SimpleNamespace

import pytest

from harness.demos import demos_dir


def test_collatz_rejects_a_mismatched_sequence(monkeypatch):
    class Failure(Exception):
        def __init__(self, kind, message):
            super().__init__(message)
            self.kind = kind

    def send(_):
        pytest.fail("A mismatch must fail before the Card sends a view message")

    monkeypatch.setitem(sys.modules, "tsugumi", SimpleNamespace(Failure=Failure, Markdown=str, send=send))
    monkeypatch.setitem(sys.modules, "card", SimpleNamespace(Input=SimpleNamespace, Output=SimpleNamespace))
    monkeypatch.setitem(sys.modules, "collatz.Sequence", SimpleNamespace(SequenceT=SimpleNamespace))
    monkeypatch.setitem(sys.modules, "collatz.Verdict", SimpleNamespace(VerdictT=SimpleNamespace))
    source = demos_dir() / "ja/collatz/definitions/compare/src/main.py"
    values = (3, 10, 5, 16, 8, 4, 2, 1)
    inputs = SimpleNamespace(**{port: SimpleNamespace(values=values) for port in ("cpp", "python", "julia", "r")})
    run = runpy.run_path(str(source))["run"]
    for port in ("python", "julia", "r"):
        mismatched = SimpleNamespace(**{**vars(inputs), port: SimpleNamespace(values=(0,))})
        with pytest.raises(Failure) as failure:
            run(mismatched)
        assert failure.value.kind == "output_invalid"
