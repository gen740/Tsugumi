"""The checked-in outputs of the template generators in ui/templates are what
the generators write (no daemon, no Nix, no plotting libraries)."""
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIBRARIES = ['matplotlib', 'plotly', 'altair']
KINDS = ['line', 'scatter', 'histogram']


def generator():
    spec = importlib.util.spec_from_file_location('visualization_templates', ROOT / 'ui/templates/visualization.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_shared_templates_are_current():
    """The checked-in outputs are what the generator writes, and each library's
    template carries its selection code and the same params and contract."""
    module = generator()
    for path, value in module.outputs().items():
        assert path.read_text() == value, f'{path.relative_to(ROOT)} is stale: run python3 ui/templates/visualization.py'
    templates = module.templates()
    assert set(templates) == {f'visualization_{library}' for library in LIBRARIES}
    defaults = [{'name': n, 'type': t, 'data': v} for n, t, v in module.PARAMS]
    for library in LIBRARIES:
        files = templates[f'visualization_{library}']
        assert set(files) == {'_card.json', '_meta.json', '_outputs.fbs', 'default.nix', 'src/main.py'}
        card = json.loads(files['_card.json'])
        assert card['params'] == defaults and card['retainArtifact'] and card['artifactOutput'] == 'figure'
        source = files['src/main.py']
        assert '# @select@' not in source and '# @insert@' not in source
        assert 'def select(' in source and 'def marks(' in source and 'def main(ctx)' in source
        compile(source, f'{library}.py', 'exec')
    # The demo's three figures are the Matplotlib template with their own kind and title.
    for language in ('en', 'ja'):
        for kind in KINDS:
            definition = ROOT / f'demos/{language}/visualization/definitions/{kind}'
            card = json.loads((definition / '_card.json').read_text())
            assert [(p['name'], p['type']) for p in card['params']] == [(n, t) for n, t, _ in module.PARAMS]
            values = {p['name']: p['data'] for p in card['params']}
            assert values['kind'] == kind and values['max_points'] == 5000 and values['sample_every'] == 1
            assert card['retainArtifact'] and card['artifactOutput'] == 'figure'
            assert (definition / '_outputs.fbs').read_text() == templates['visualization_matplotlib']['_outputs.fbs']


def test_shared_marimo_template_is_current():
    spec=importlib.util.spec_from_file_location('marimo_template',ROOT/'ui/templates/marimo.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    for path,value in module.outputs().items():assert path.read_text()==value
    definition=json.loads(module.files()['_card.json'])
    assert definition['reuseWorker'] is False and definition['retainArtifact'] is True
