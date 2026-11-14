"""Run both comparison Cards and CI's reduced demo without Nix or RPC."""
import ast
import json
from pathlib import Path
import runpy
import shutil
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize("language", ["en", "ja"])
def test_collatz_compare_and_ci_reduction(tmp_path, monkeypatch, language):
    class Failure(Exception):
        def __init__(self, kind, message):
            super().__init__(message)
            self.kind = kind

    monkeypatch.setitem(sys.modules, "tsugumi", SimpleNamespace(Failure=Failure))
    project = tmp_path / "collatz"
    shutil.copytree(ROOT / "demos" / language / "collatz", project)
    source = project / "definitions/compare/src/main.py"
    values = [3, 10, 5, 16, 8, 4, 2, 1]
    inputs = SimpleNamespace(**{port: SimpleNamespace(values=tuple(values)) for port in ("cpp", "python", "julia", "r")})
    reports = []
    report = SimpleNamespace(markdown=reports.append)
    main = runpy.run_path(str(source))["main"]
    expected = {"verdict": {"equal": True, "count": len(values)}, "sequence": {"values": values}}
    assert main(inputs, report) == expected
    assert "C++, Python, Julia and R agree" in reports[-1]
    for port in ("python", "julia", "r"):
        mismatched = SimpleNamespace(**{**vars(inputs), port: SimpleNamespace(values=(0,))})
        with pytest.raises(Failure, match="sequences differ") as failure:
            main(mismatched, report)
        assert failure.value.kind == "output_invalid"

    # Execute the actual fixture's transformation; its exact source assertions
    # must stay in sync with the runnable demo in both locales.
    module = ast.parse((ROOT / "tests/python/test_demos.py").read_text())
    helpers = [node for node in module.body if isinstance(node, ast.FunctionDef) and node.name in ("set_param", "without_julia_and_r")]
    namespace = {"json": json, "shutil": shutil}
    exec(compile(ast.Module(body=helpers, type_ignores=[]), "test_demos.py", "exec"), namespace)
    namespace["without_julia_and_r"](project)
    reduced = runpy.run_path(str(source))["main"]
    assert reduced(SimpleNamespace(cpp=inputs.cpp, python=inputs.python), report) == expected
    assert "C++ and Python agree" in reports[-1]
    assert {port["name"] for port in json.loads((source.parents[1] / "_card.json").read_text())["inputs"]} == {"cpp", "python"}
