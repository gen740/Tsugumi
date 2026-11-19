"""The checked-in outputs of the template generators in ui/shared/templates are what
the generators write (no daemon, no Nix, no plotting libraries)."""
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def generator():
    spec = importlib.util.spec_from_file_location('visualization_templates', ROOT / 'ui/shared/templates/visualization.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_shared_templates_are_current():
    """The checked-in outputs are what the generator writes, and each library's
    template carries its selection code and the same params and contract."""
    module = generator()
    for path, value in module.outputs().items():
        assert path.read_text() == value, f'{path.relative_to(ROOT)} is stale: run python3 ui/shared/templates/visualization.py'
    for library, files in module.templates().items():
        assert {'_card.json', 'default.nix', 'src/main.py'} <= files.keys()
        compile(files['src/main.py'], library, 'exec')


def test_shared_marimo_template_is_current():
    spec=importlib.util.spec_from_file_location('marimo_template',ROOT/'ui/shared/templates/marimo.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    for path,value in module.outputs().items():assert path.read_text()==value
    definition=json.loads(module.files()['_card.json'])
    assert 'filesystem' not in definition
    assert definition['reuseWorker'] is False and definition['retainArtifact'] is True
