"""Run one notebook with values from the Card and save its selected result."""
from html import escape
from importlib.util import module_from_spec, spec_from_file_location
import json
import math
from pathlib import Path
from shutil import copyfile

from card import Artifact, Input, Output, params
from notebook.Number import NumberT
from tsugumi import Failure


def run(input: Input) -> Output:
    source_root = Path(__file__).resolve().parent
    source = (source_root / params.notebook).resolve()
    if not source.is_relative_to(source_root) or source.suffix != ".py":
        raise Failure("input_invalid", "select a Python notebook in the Card's source directory")
    definitions = {}
    for name, binding in json.loads(params.bindings).items():
        origin, *parts = binding.split(".")
        if origin == "inputs":
            value = input.require(parts.pop(0))
        elif origin == "param":
            value = params
        else:
            raise Failure("input_invalid", f"unknown binding origin {origin}")
        for field in parts:
            value = getattr(value, field)
        definitions[name] = value

    spec = spec_from_file_location("card_notebook", source)
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    _, values = module.app.run(defs=definitions)
    value = values[params.selected]
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise Failure("output_invalid", "the selected notebook result must be a finite number")

    artifact, directory = Artifact.dir()
    (directory / "result.json").write_text(json.dumps({params.selected: value}), encoding="utf-8")
    (directory / "bindings.json").write_text(json.dumps(definitions), encoding="utf-8")
    source_dir = directory / "source"
    source_dir.mkdir()
    copyfile(source, source_dir / source.name)
    copyfile(__file__, source_dir / Path(__file__).name)
    (directory / "index.html").write_text(
        f'<!doctype html><meta charset="utf-8"><h1>Notebook result</h1>'
        f'<p>{escape(params.selected)}: {value:g}</p><p><a href="source/{escape(source.name)}">Source</a></p>',
        encoding="utf-8",
    )
    # TODO(open): Port the demo display.
    return Output(value=NumberT(value=value), report=artifact)
