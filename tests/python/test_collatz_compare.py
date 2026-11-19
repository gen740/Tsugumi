"""Check that a disagreement reaches the Collatz Card’s failure path."""
from pathlib import Path
import runpy
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_collatz_rejects_a_mismatched_sequence(monkeypatch):
    class Failure(Exception):
        def __init__(self, kind, message):
            super().__init__(message)
            self.kind = kind

    monkeypatch.setitem(sys.modules, "tsugumi", SimpleNamespace(Failure=Failure))
    source = ROOT / "demos/ja/collatz/definitions/compare/src/main.py"
    values = (3, 10, 5, 16, 8, 4, 2, 1)
    inputs = SimpleNamespace(**{port: SimpleNamespace(values=values) for port in ("cpp", "python", "julia", "r")})
    main = runpy.run_path(str(source))["main"]
    for port in ("python", "julia", "r"):
        mismatched = SimpleNamespace(**{**vars(inputs), port: SimpleNamespace(values=(0,))})
        with pytest.raises(Failure) as failure:
            main(mismatched, None)
        assert failure.value.kind == "output_invalid"
