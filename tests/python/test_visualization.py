"""Visualization templates: the generated Card sources, run as they are.

Each test runs a template's generated src/main.py (ui/templates/visualization.py)
in a fresh interpreter with the real SDK helper, FlatBuffers bindings and
plotting libraries; only the worker host is replaced. No UI process, browser or
daemon is started here (tests/python/test_demo_visualization.py runs one).
"""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import textwrap

import pytest

ROOT = Path(__file__).resolve().parents[2]
LIBRARIES = ['matplotlib', 'plotly', 'altair']
KINDS = ['line', 'scatter', 'histogram']
FILES = {'matplotlib': {'plot.png', 'plot.svg', 'plot.pdf'}, 'plotly': {'plot.json', 'plot.html'}, 'altair': {'plot.json', 'plot.html'}}


def generator():
    spec = importlib.util.spec_from_file_location('visualization_templates', ROOT / 'ui/templates/visualization.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def flatc(tmp_path, name, schema):
    (tmp_path / name).write_text(schema)
    subprocess.run([os.environ.get('TSUGUMI_FLATC', 'flatc'), '--python', '--gen-object-api', '-o', str(tmp_path), str(tmp_path / name)], check=True)


# The worker's host functions the SDK calls at import; the Context below
# answers everything else a Card asks for.
HOST = '''
def inputs(): return []
def info(): return {}
def environ(): return list(__import__('os').environb.items())
def cancelled(): return False
def remaining_ms(): return 60000
def display_image(*args): pass
def display_module(*args): pass
'''

PRELUDE = '''
import csv, json, sys, importlib.util
from pathlib import Path
import flatbuffers
import tsugumi
from tsugumi import contract
HERE = Path(sys.argv[1])
TEMPLATES = json.loads((HERE / 'templates.json').read_text())

def camel(name): return ''.join(part[:1].upper() + part[1:] for part in name.split('_'))

class Params:
    """The template's params with its defaults (_card.json), as the generated accessors return them."""
    def __init__(self, library, **changes):
        card = json.loads(TEMPLATES['visualization_' + library]['_card.json'])
        self.values = {camel(p['name']): p['data'] for p in card['params']} | changes
    def __getattr__(self, name):
        value = self.values[name]
        return lambda: value.encode() if isinstance(value, str) else value

class Context:
    run_id = '12345678-1234-1234-1234-123456789abc'
    card = 'chart'
    def __init__(self, library, raw=None, **params):
        self.library, self.raw, self.p = library, raw, Params(library, **params)
        self.invocation = 1; self.output_directory = str(HERE / 'out'); self.stop = False; self.deadline = 60000
        self.shown = []; self.outputs = {}
    def params(self): return self.p
    def input(self, port, cls): return cls.GetRootAs(self.raw, 0)
    def cancelled(self): return self.stop
    def remaining_ms(self): return self.deadline
    def display_figure(self, figure): self.shown.append(type(figure).__name__)
    builder = tsugumi.Context.builder
    def output(self, port, builder, root): builder.Finish(root); self.outputs[port] = bytes(builder.Output())
    def run(self, **params):
        self.p.values |= {camel(k): v for k, v in params.items()}
        load(self.library).main(self)
        from outputs.figure.visualization.Files import Files
        return Path(Files.GetRootAs(self.outputs['figure'], 0).Value().decode())

def load(library):
    """The template's generated src/main.py, as a Card's main module."""
    path = HERE / f'main_{library}.py'
    path.write_text(TEMPLATES['visualization_' + library]['src/main.py'])
    spec = importlib.util.spec_from_file_location(f'main_{library}', path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module

def use(kind, typename='Table'):
    (HERE / '_contract.json').write_text(json.dumps({'inputs': [{'name': 'data', 'type': 'inputs.data.example.' + typename, 'kind': kind, 'required': True}], 'outputs': []}))
    contract._contract.cache_clear()

def table(x, y, group=None):
    """The `data` input as typed equally sized columns (inputs.data.example.Table)."""
    from inputs.data.example.Table import TableT
    value = TableT(); value.x = list(x); value.y = list(y); value.group = group
    b = flatbuffers.Builder(1024); b.Finish(value.Pack(b)); use('flatbuffer')
    return bytes(b.Output())

def expect_failure(kind, fn, *words):
    try: fn()
    except tsugumi.Failure as e:
        assert e.kind == kind, (e.kind, e.message)
        for word in words: assert word in e.message, e.message
        return e
    raise AssertionError(f'expected a {kind} failure')

def rows_of(path):
    with path.open(newline='') as f: return list(csv.DictReader(f))
'''


def run(tmp_path, code):
    bindings = tmp_path / 'bindings'
    if not bindings.exists():
        flatc(tmp_path, '_outputs.fbs', 'namespace outputs.figure.visualization; table Files { value:string; } root_type Files;')
        flatc(tmp_path, 'data.fbs', 'namespace inputs.data.example; table Row { x:double; y:double; } '
                                    'table Rows { rows:[Row]; } table Table { x:[double]; y:[double]; group:[string]; } '
                                    'table File { value:string; } root_type Table;')
        bindings.touch()
        # The SDK reads _contract.json beside its package, as in a Card's build.
        shutil.copytree(ROOT / 'sdk/python/tsugumi', tmp_path / 'tsugumi', ignore=shutil.ignore_patterns('__pycache__'))
        (tmp_path / '_tsugumi_host.py').write_text(HOST)
        (tmp_path / 'templates.json').write_text(json.dumps(generator().templates()))
    env = {**os.environ, 'PYTHONPATH': str(tmp_path), 'MPLBACKEND': 'Agg',
           'MPLCONFIGDIR': str(tmp_path / 'mpl'), 'PYTHONDONTWRITEBYTECODE': '1'}
    script = tmp_path / 'run.py'
    script.write_text(PRELUDE + '\n' + textwrap.dedent(code))
    result = subprocess.run([sys.executable, str(script), str(tmp_path)], env=env, text=True, capture_output=True, timeout=180)
    assert result.returncode == 0, result.stdout + result.stderr
    return result


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


@pytest.mark.parametrize('library', LIBRARIES)
@pytest.mark.parametrize('kind', KINDS)
def test_generated_template_draws_saves_and_redraws(tmp_path, library, kind):
    """Defaults draw every finite row; non-finite rows are counted, not drawn;
    files and metadata belong to the invocation that wrote them."""
    run(tmp_path, f'''
import matplotlib.pyplot as plt
xs = [float(i) for i in range(24)] + [float('nan'), 3.0]
ys = [float(i * i) for i in range(24)] + [1.0, float('inf')]
ctx = Context({library!r}, table(xs, ys, ['a' if i % 2 else 'b' for i in range(26)]), Kind={kind!r}, Series='group')
folder = ctx.run()
assert folder.name == 'figure' and folder.parent == Path(ctx.output_directory)
assert set({sorted(FILES[library])!r}) | {{'plotted.csv', 'metadata.json'}} == {{p.name for p in folder.iterdir()}}
meta = json.loads((folder / 'metadata.json').read_text())
finite = 25 if {kind!r} == 'histogram' else 24  # a histogram needs only x
assert meta['run_id'] == ctx.run_id and meta['card'] == 'chart' and meta['invocation'] == 1
assert meta['library'] == {library!r} and meta['kind'] == {kind!r}
assert (meta['source_rows'], meta['finite_rows'], meta['sampled'], meta['sample_every'], meta['max_points']) == (26, finite, False, 1, 5000)
assert meta['drawn_rows'] == finite and meta['note'].startswith(f'{{finite}} of 26 rows')
plotted = rows_of(folder / 'plotted.csv')
if {kind!r} == 'histogram':
    assert set(plotted[0]) == {{'left', 'right', 'count', 'series'}}
    assert sum(int(r['count']) for r in plotted) == finite
else:
    assert set(plotted[0]) == {{'x', 'y', 'series'}} and len(plotted) == 24
    assert {{(float(r['x']), float(r['y'])) for r in plotted}} == {{(float(i), float(i * i)) for i in range(24)}}
    assert {{r['series'] for r in plotted}} == {{'a', 'b'}}
assert len(ctx.shown) == 1  # one display per invocation
artifact = folder / ('plot.png' if {library!r} == 'matplotlib' else 'plot.json')
before = artifact.read_bytes()
# The next invocation writes the same folder again: its own figure and metadata.
ctx.invocation = 2; ctx.raw = table(xs, [y + 7 for y in ys], None)
ctx.run(Series='', Theme='dark', Title='Updated')
assert artifact.read_bytes() != before
meta = json.loads((folder / 'metadata.json').read_text())
assert meta['invocation'] == 2 and {{r['series'] for r in rows_of(folder / 'plotted.csv')}} == {{''}}
assert plt.get_fignums() == []
if {library!r} == 'matplotlib':
    from PIL import Image
    assert Image.open(artifact).size == (7 * 120, 4 * 120)
    assert (folder / 'plot.pdf').read_bytes().startswith(b'%PDF')
else:
    html = (folder / 'plot.html').read_text()
    assert '<script src=' not in html and 'cdn.jsdelivr.net' not in html.split('<body>')[0]
''')


@pytest.mark.parametrize('library', LIBRARIES)
def test_default_max_points_fails_and_sampling_is_declared(tmp_path, library):
    """More points than max_points (5000 by default) stop the Card instead of
    being thinned; sample_every thins on purpose and says so everywhere."""
    run(tmp_path, f'''
n = 6001
raw = table(range(n), [i % 17 for i in range(n)])
ctx = Context({library!r}, raw)
for kind in ('line', 'scatter', 'histogram'):
    expect_failure('validation_failed', lambda: ctx.run(Kind=kind), '6001 points', 'max_points (5000)', 'sample_every')
    assert not (Path(ctx.output_directory) / 'figure').exists() and 'figure' not in ctx.outputs
folder = ctx.run(Kind='scatter', SampleEvery=2)
meta = json.loads((folder / 'metadata.json').read_text())
assert (meta['finite_rows'], meta['drawn_rows'], meta['sampled'], meta['sample_every']) == (6001, 3001, True, 2)
assert 'SAMPLED: 1 point in 2 drawn (3001 of 6001)' in meta['note']
plotted = rows_of(folder / 'plotted.csv')
assert [float(r['x']) for r in plotted] == [float(i) for i in range(0, n, 2)]
if {library!r} != 'matplotlib':
    assert 'SAMPLED' in (folder / 'plot.json').read_text()
# Raising max_points draws every point without sampling.
meta = json.loads((ctx.run(Kind='line', SampleEvery=1, MaxPoints=n) / 'metadata.json').read_text())
assert (meta['drawn_rows'], meta['sampled']) == (n, False)
# A sampled histogram counts the selected rows and labels those bin counts.
folder = ctx.run(Kind='histogram', SampleEvery=3, MaxPoints=5000)
meta = json.loads((folder / 'metadata.json').read_text())
assert (meta['drawn_rows'], meta['sampled']) == (2001, True)
assert 'bin counts describe the sample' in meta['note']
assert sum(int(r['count']) for r in rows_of(folder / 'plotted.csv')) == 2001
if {library!r} != 'matplotlib':
    assert 'bin counts describe the sample' in (folder / 'plot.json').read_text()
expect_failure('validation_failed', lambda: ctx.run(Kind='pie', SampleEvery=1), 'kind')
expect_failure('validation_failed', lambda: ctx.run(Kind='line', Y='missing'), "'missing'")
''')


def test_empty_cancelled_and_deadline(tmp_path):
    run(tmp_path, '''
for library in ['matplotlib', 'plotly', 'altair']:
    ctx = Context(library, table([], []))
    meta = json.loads((ctx.run() / 'metadata.json').read_text())
    assert (meta['source_rows'], meta['drawn_rows']) == (0, 0)
ctx = Context('matplotlib', table([1.0], [2.0]))
ctx.stop = True
expect_failure('cancelled', ctx.run)
ctx.stop = False; ctx.deadline = 0
expect_failure('timeout', ctx.run)
''')


def test_typed_rows_csv_file_and_directory_inputs(tmp_path):
    """input_rows reads typed rows (data_path), File CSV and a Directory member."""
    run(tmp_path, '''
from tsugumi.visualization import input_rows
from inputs.data.example.Rows import RowsT
from inputs.data.example.Row import RowT
from inputs.data.example.File import FileT
def pack(value):
    b = flatbuffers.Builder(256); b.Finish(value.Pack(b)); return bytes(b.Output())
rows = RowsT(); rows.rows = []
for x, y in [(1., 3.), (2., 4.)]:
    r = RowT(); r.x, r.y = x, y; rows.rows.append(r)
use('flatbuffer', 'Rows')
ctx = Context('matplotlib', pack(rows), DataPath='rows')
assert input_rows(ctx, field='rows') == [{'x': 1., 'y': 3.}, {'x': 2., 'y': 4.}]
expect_failure('validation_failed', lambda: input_rows(ctx, field='other'), 'other')
meta = json.loads((ctx.run() / 'metadata.json').read_text())
assert meta['drawn_rows'] == 2
source = HERE / 'data.csv'
source.write_text('x,y\\n1,3\\n2,4\\nbad,5\\n')
file = FileT(); file.value = str(source)
use('file', 'File')
ctx = Context('matplotlib', pack(file))
assert input_rows(ctx)[1] == {'x': '2', 'y': '4'}
meta = json.loads((ctx.run() / 'metadata.json').read_text())
assert (meta['source_rows'], meta['finite_rows']) == (3, 2)
file.value = str(HERE)
use('directory', 'File')
ctx = Context('matplotlib', pack(file))
assert input_rows(ctx, member='data.csv')[0]['y'] == '3'
expect_failure('validation_failed', lambda: input_rows(ctx, member='../outside.csv'), 'relative member')
(HERE / '_contract.json').write_text(json.dumps({'inputs': [], 'outputs': []}))
contract._contract.cache_clear()
expect_failure('validation_failed', lambda: input_rows(ctx), 'Connect')
''')
