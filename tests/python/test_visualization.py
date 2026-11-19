"""Visualization templates: the generated Card sources, run as they are.

Each library runs its generated src/main.py (ui/shared/templates/visualization.py)
in one interpreter with the real SDK helper, FlatBuffers bindings and
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
    spec = importlib.util.spec_from_file_location('visualization_templates', ROOT / 'ui/shared/templates/visualization.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The worker's host functions the SDK calls at import; the InvocationFixture below
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
import csv, json, sys, importlib.util, shutil
from types import SimpleNamespace
from html.parser import HTMLParser
from pathlib import Path
import flatbuffers
import tsugumi
from tsugumi import contract
HERE = Path(sys.argv[1])
TEMPLATES = json.loads((HERE / 'templates.json').read_text())

class Scripts(HTMLParser):
    def __init__(self):
        super().__init__()
        self.sources = []
    def handle_starttag(self, tag, attributes):
        if tag == 'script':
            self.sources.append(dict(attributes).get('src'))

def shows_note(folder, library, note):
    plot = json.loads((folder / 'plot.json').read_text())
    if library == 'plotly':
        return note in [label.get('text') for label in plot['layout'].get('annotations', [])]
    return note in plot.get('title', {}).get('subtitle', [])

def camel(name): return ''.join(part[:1].upper() + part[1:] for part in name.split('_'))

class Params:
    """The template's params with its defaults (_card.json), as the generated accessors return them."""
    def __init__(self, library, **changes):
        card = json.loads(TEMPLATES['visualization_' + library]['_card.json'])
        self.values = {camel(p['name']): p['data'] for p in card['params']} | changes
    def __getattr__(self, name):
        value = self.values[name[:1].upper() + name[1:]]
        return value.encode() if isinstance(value, str) else value

class InvocationFixture:
    run_id = '12345678-1234-1234-1234-123456789abc'
    card = 'chart'
    def __init__(self, library, raw=None, **params):
        self.library, self.raw, self.p = library, raw, Params(library, **params)
        self.invocation = 1; self.output_directory = str(HERE / 'out'); self.stop = False; self.deadline = 60000
        self.shown = []; self.outputs = {}
    def input(self, port, cls): return cls.GetRootAs(self.raw, 0)
    def cancelled(self): return self.stop
    def remaining_ms(self): return self.deadline
    def figure(self, figure): self.shown.append(type(figure).__name__)
    def run(self, **params):
        self.p.values |= {camel(k): v for k, v in params.items()}
        inputs = SimpleNamespace(params=self.p, input=self.input)
        report = SimpleNamespace(figure=self.figure, log=lambda message: None)
        result = load(self.library).main(inputs, report, self)
        self.outputs = {port: tsugumi.message(value) for port, value in result.items()}
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
    ports = [{'name': 'data', 'type': 'inputs.data.example.' + typename, 'kind': kind, 'required': True}] if kind else []
    (HERE / '_contract.json').write_text(json.dumps({'inputs': ports, 'outputs': []}))
    contract._contract.cache_clear()
    contract._port_index.cache_clear()
    contract._generated.cache_clear()

def table(x, y, group=None):
    """The `data` input as typed equally sized columns (inputs.data.example.Table)."""
    from inputs.data.example.Table import TableT
    value = TableT(); value.x = list(x); value.y = list(y); value.group = group
    b = flatbuffers.Builder(1024); b.Finish(value.Pack(b)); use('flatbuffer')
    return bytes(b.Output())

def expect_failure(kind, fn):
    try: fn()
    except tsugumi.Failure as e:
        assert e.kind == kind, (e.kind, e.message)
        return e
    raise AssertionError(f'expected a {kind} failure')

def rows_of(path):
    with path.open(newline='') as f: return list(csv.DictReader(f))
'''


@pytest.fixture(scope="module")
def bindings(tmp_path_factory):
    root = tmp_path_factory.mktemp("visualization")
    (root / "_outputs.fbs").write_text(
        "namespace outputs.figure.visualization; table Files { value:string; } root_type Files;")
    (root / "data.fbs").write_text(
        "namespace inputs.data.example; table Row { x:double; y:double; } "
        "table Rows { rows:[Row]; } table Table { x:[double]; y:[double]; group:[string]; } "
        "table File { value:string; } root_type Table;")
    subprocess.run([os.environ.get("TSUGUMI_FLATC", "flatc"), "--python", "--gen-object-api",
                    "-o", str(root), str(root / "_outputs.fbs"), str(root / "data.fbs")], check=True)
    shutil.copytree(ROOT / "sdk/python/tsugumi", root / "tsugumi", ignore=shutil.ignore_patterns("__pycache__"))
    (root / "_tsugumi_host.py").write_text(HOST)
    (root / "templates.json").write_text(json.dumps(generator().templates()))
    return root


def run(tmp_path, bindings, cases):
    shutil.copytree(bindings, tmp_path, dirs_exist_ok=True)
    sections = []
    for name, body in cases:
        sections.append("def " + name + "():\n" + textwrap.indent(textwrap.dedent(body), "    "))
        sections.append(f"{name}()\nshutil.rmtree(HERE / 'out', ignore_errors=True)")
    env = {**os.environ, 'PYTHONPATH': str(tmp_path), 'MPLBACKEND': 'Agg',
           'MPLCONFIGDIR': str(tmp_path / 'mpl'), 'PYTHONDONTWRITEBYTECODE': '1'}
    script = tmp_path / 'run.py'
    script.write_text(PRELUDE + '\n\n'.join(sections))
    result = subprocess.run([sys.executable, str(script), str(tmp_path)], env=env, text=True, capture_output=True, timeout=180)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize("library", LIBRARIES)
def test_generated_visualization_templates(tmp_path, bindings, library):
    cases = [(f"draw_save_redraw_{kind}", draws_saves_and_redraws(library, kind)) for kind in KINDS]
    cases += [("max_points_and_sampling", max_points_and_sampling(library)),
              ("empty_input", empty_input(library))]
    if library == "matplotlib":
        cases += [("cancel_and_deadline", CANCEL_AND_DEADLINE), ("input_kinds", INPUT_KINDS)]
    run(tmp_path, bindings, cases)


def draws_saves_and_redraws(library, kind):
    """Defaults draw every finite row; non-finite rows are counted, not drawn;
    files and metadata belong to the invocation that wrote them."""
    return f'''
import matplotlib.pyplot as plt
xs = [float(i) for i in range(24)] + [float('nan'), 3.0]
ys = [float(i * i) for i in range(24)] + [1.0, float('inf')]
case = InvocationFixture({library!r}, table(xs, ys, ['a' if i % 2 else 'b' for i in range(26)]), Kind={kind!r}, Series='group')
folder = case.run()
assert folder.name == 'figure' and folder.parent == Path(case.output_directory)
assert set({sorted(FILES[library])!r}) | {{'plotted.csv', 'metadata.json'}} <= {{p.name for p in folder.iterdir()}}
meta = json.loads((folder / 'metadata.json').read_text())
finite = 25 if {kind!r} == 'histogram' else 24  # a histogram needs only x
assert meta['run_id'] == case.run_id and meta['card'] == 'chart' and meta['invocation'] == 1
assert meta['library'] == {library!r} and meta['kind'] == {kind!r}
assert (meta['source_rows'], meta['finite_rows'], meta['sampled'], meta['sample_every'], meta['max_points']) == (26, finite, False, 1, 5000)
assert meta['drawn_rows'] == finite and meta['note']
plotted = rows_of(folder / 'plotted.csv')
if {kind!r} == 'histogram':
    assert set(plotted[0]) >= {{'left', 'right', 'count', 'series'}}
    assert sum(int(r['count']) for r in plotted) == finite
else:
    assert set(plotted[0]) >= {{'x', 'y', 'series'}} and len(plotted) == 24
    assert {{(float(r['x']), float(r['y'])) for r in plotted}} == {{(float(i), float(i * i)) for i in range(24)}}
    assert {{r['series'] for r in plotted}} == {{'a', 'b'}}
assert len(case.shown) == 1  # one display per invocation
artifact = folder / ('plot.png' if {library!r} == 'matplotlib' else 'plot.json')
if {library!r} == 'matplotlib':
    from PIL import Image, ImageChops
    before_image = Image.open(artifact).convert('RGB')
# The next invocation writes the same folder again: its own figure and metadata.
case.invocation = 2; case.raw = table(xs, [y + 7 for y in ys], None)
case.run(Series='', Theme='dark', Title='Updated')
updated = rows_of(folder / 'plotted.csv')
assert updated != plotted
meta = json.loads((folder / 'metadata.json').read_text())
assert meta['invocation'] == 2 and {{r['series'] for r in rows_of(folder / 'plotted.csv')}} == {{''}}
assert plt.get_fignums() == []
if {library!r} == 'matplotlib':
    rendered = Image.open(artifact).convert('RGB')
    assert ImageChops.difference(before_image, rendered).getbbox()
    assert (folder / 'plot.pdf').read_bytes().startswith(b'%PDF')
else:
    spec = json.loads(artifact.read_text())
    title = spec['layout']['title']['text'] if {library!r} == 'plotly' else spec['title']['text']
    assert title == 'Updated'
    html = Scripts()
    html.feed((folder / 'plot.html').read_text())
    assert html.sources and all(source is None or source.startswith('data:') for source in html.sources)
'''


def max_points_and_sampling(library):
    """More points than max_points (5000 by default) stop the Card instead of
    being thinned; sample_every thins on purpose and says so everywhere."""
    return f'''
oversized = InvocationFixture({library!r}, table(range(5001), range(5001)))
for kind in ('line', 'scatter', 'histogram'):
    expect_failure('validation_failed', lambda: oversized.run(Kind=kind))
    assert not (Path(oversized.output_directory) / 'figure').exists() and 'figure' not in oversized.outputs

# Exercise the same limit and sampling paths with a low configured cap.
n = 13
case = InvocationFixture({library!r}, table(range(n), range(n)), MaxPoints=8)
expect_failure('validation_failed', lambda: case.run(Kind='line'))
folder = case.run(Kind='scatter', SampleEvery=2)
meta = json.loads((folder / 'metadata.json').read_text())
assert (meta['finite_rows'], meta['drawn_rows'], meta['sampled'], meta['sample_every']) == (13, 7, True, 2)
assert meta['note']
plotted = rows_of(folder / 'plotted.csv')
assert [float(r['x']) for r in plotted] == [float(i) for i in range(0, n, 2)]
if {library!r} != 'matplotlib':
    assert shows_note(folder, {library!r}, meta['note'])
# Raising max_points draws every point without sampling.
meta = json.loads((case.run(Kind='line', SampleEvery=1, MaxPoints=n) / 'metadata.json').read_text())
assert (meta['drawn_rows'], meta['sampled']) == (n, False)
# A sampled histogram counts the selected rows and labels those bin counts.
folder = case.run(Kind='histogram', SampleEvery=3, MaxPoints=8)
meta = json.loads((folder / 'metadata.json').read_text())
assert (meta['drawn_rows'], meta['sampled']) == (5, True)
assert meta['note']
assert sum(int(r['count']) for r in rows_of(folder / 'plotted.csv')) == 5
if {library!r} != 'matplotlib':
    assert shows_note(folder, {library!r}, meta['note'])
expect_failure('validation_failed', lambda: case.run(Kind='pie', SampleEvery=1))
expect_failure('validation_failed', lambda: case.run(Kind='line', Y='missing'))
'''


def empty_input(library):
    return f'''
case = InvocationFixture({library!r}, table([], []))
meta = json.loads((case.run() / 'metadata.json').read_text())
assert (meta['source_rows'], meta['drawn_rows']) == (0, 0)
'''


CANCEL_AND_DEADLINE = '''
case = InvocationFixture('matplotlib', table([1.0], [2.0]))
case.stop = True
expect_failure('cancelled', case.run)
case.stop = False; case.deadline = 0
expect_failure('timeout', case.run)
'''


# input_rows reads typed rows, File CSV and a Directory member.
INPUT_KINDS = '''
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
case = InvocationFixture('matplotlib', pack(rows), DataPath='rows')
assert input_rows(SimpleNamespace(input=case.input), case, field='rows') == [{'x': 1., 'y': 3.}, {'x': 2., 'y': 4.}]
expect_failure('validation_failed', lambda: input_rows(SimpleNamespace(input=case.input), case, field='other'))
meta = json.loads((case.run() / 'metadata.json').read_text())
assert meta['drawn_rows'] == 2
source = HERE / 'data.csv'
source.write_text('x,y\\n1,3\\n2,4\\nbad,5\\n')
file = FileT(); file.value = str(source)
use('file', 'File')
case = InvocationFixture('matplotlib', pack(file))
assert input_rows(SimpleNamespace(input=case.input), case)[1] == {'x': '2', 'y': '4'}
meta = json.loads((case.run() / 'metadata.json').read_text())
assert (meta['source_rows'], meta['finite_rows']) == (3, 2)
file.value = str(HERE)
use('directory', 'File')
case = InvocationFixture('matplotlib', pack(file))
assert input_rows(SimpleNamespace(input=case.input), case, member='data.csv')[0]['y'] == '3'
expect_failure('validation_failed', lambda: input_rows(SimpleNamespace(input=case.input), case, member='../outside.csv'))
use(None)
expect_failure('validation_failed', lambda: input_rows(SimpleNamespace(input=case.input), case))
'''
