"""Exercise scaffold bindings without marimo, a worker or generated FlatBuffers.

Only the contract's decoded objects and the notebook runner are doubles. The
generated main and the SDK's binding resolver run unchanged; serialization,
notebook execution and output encoding belong to the SDK/demo integration tests.
"""
import importlib.util
import json
from pathlib import Path
import runpy
import shutil
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Failure(Exception):
    def __init__(self, kind, message):
        super().__init__(message)
        self.kind = kind


@pytest.fixture
def scaffold(tmp_path, monkeypatch):
    shutil.copytree(ROOT / 'demos/en/marimo/definitions/notebook', tmp_path, dirs_exist_ok=True)
    card = json.loads((tmp_path / '_card.json').read_text())
    params = {p['name']: p['data'].encode() if p['type'] == 'string' else p['data']
              for p in card['params']}
    inputs = SimpleNamespace(data=SimpleNamespace(value=7.0), params=SimpleNamespace(**params))
    contract = SimpleNamespace(
        ports=lambda: card['inputs'],
        input_object=lambda inputs, name: getattr(inputs, name),
        params_object=lambda inputs: inputs.params,
    )
    monkeypatch.syspath_prepend(str(ROOT / 'sdk/python/tsugumi'))
    adapter = load('notebook_adapter', ROOT / 'sdk/python/tsugumi/notebook.py')
    monkeypatch.setitem(sys.modules, 'tsugumi', SimpleNamespace(Failure=Failure, contract=contract))
    monkeypatch.setitem(sys.modules, 'tsugumi.notebook', adapter)
    return tmp_path, inputs, adapter


def test_scaffold_bindings_and_returned_outputs(scaffold, monkeypatch):
    root, inputs, adapter = scaffold
    report, run = object(), object()
    folder = root / 'output/notebook'

    def run_notebook(report, run, source, notebook, definitions, selected):
        return {'result': definitions['source'] * definitions['factor']}, folder

    monkeypatch.setattr(adapter, 'run', run_notebook)
    main = runpy.run_path(str(root / 'src/main.py'))['main']
    inputs.params.factor = 3
    assert main(inputs, report, run) == {'value': {'value': 21.0}, 'report': {'value': str(folder)}}
