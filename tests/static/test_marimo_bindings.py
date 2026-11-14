"""Exercise scaffold bindings without marimo, a worker or generated FlatBuffers.

Only the contract's decoded objects and the notebook runner are doubles. The
generated main and the SDK's binding resolver run unchanged; serialization,
notebook execution and output encoding belong to the SDK/demo integration tests.
"""
import importlib.util
import json
from pathlib import Path
import runpy
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
    template = load('marimo_template', ROOT / 'ui/templates/marimo.py')
    for name, content in template.files().items():
        target = tmp_path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
    card = json.loads((tmp_path / '_card.json').read_text())
    params = {p['name']: p['data'].encode() if p['type'] == 'string' else p['data']
              for p in card['params']}
    inputs = SimpleNamespace(data=SimpleNamespace(value=7.0), params=SimpleNamespace(**params))
    contract = SimpleNamespace(
        ports=lambda: card['inputs'],
        input_object=lambda inputs, name: getattr(inputs, name),
        params_object=lambda inputs: inputs.params,
    )
    adapter = load('notebook_adapter', ROOT / 'sdk/python/tsugumi/notebook.py')
    monkeypatch.setitem(sys.modules, 'tsugumi', SimpleNamespace(Failure=Failure, contract=contract))
    monkeypatch.setitem(sys.modules, 'tsugumi.notebook', adapter)
    return tmp_path, inputs, adapter


def test_scaffold_default_bindings_and_returned_outputs(scaffold, monkeypatch):
    root, inputs, adapter = scaffold
    report, run = object(), object()
    folder = root / 'output/notebook'
    calls = []

    def run_notebook(actual_report, actual_run, source, notebook, definitions, selected):
        assert (actual_report, actual_run, source, notebook, selected) == (
            report, run, root / 'src', 'notebook.py', ['result'])
        calls.append(definitions)
        return {'result': definitions['source'] * definitions['factor']}, folder

    monkeypatch.setattr(adapter, 'run', run_notebook)
    main = runpy.run_path(str(root / 'src/main.py'))['main']
    assert main(inputs, report, run) == {'value': {'value': 14.0}, 'report': {'value': str(folder)}}
    assert calls == [{'source': 7.0, 'factor': 2}]
    # A direct parameter binding reads the invocation's params, not a fixed default.
    inputs.params.factor = 3
    assert main(inputs, report, run)['value'] == {'value': 21.0}
    assert calls[-1] == {'source': 7.0, 'factor': 3}


def test_singular_input_binding_is_rejected(scaffold):
    _, inputs, adapter = scaffold
    with pytest.raises(Failure, match='has no source') as error:
        adapter.bindings(inputs, {'source': 'input.data.value'})
    assert error.value.kind == 'validation_failed'
