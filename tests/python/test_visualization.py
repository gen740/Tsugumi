"""Visualization scaffolds: data semantics, exports and repeated rendering.

The worker host is replaced, but FlatBuffers bindings and all three plotting
libraries are real. No UI process or browser is launched by these tests.
"""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import textwrap

import pytest

ROOT = Path(__file__).resolve().parents[2]


def run(tmp_path, code):
    host = '''
def inputs(): return []
def info(): return {}
def environ(): return list(__import__('os').environb.items())
def cancelled(): return False
def remaining_ms(): return 60000
def display_image(*args): pass
def display_module(*args): pass
'''
    (tmp_path / '_tsugumi_host.py').write_text(host)
    prelude = '''
import json, sys, types
from pathlib import Path
from types import SimpleNamespace
import tsugumi
from tsugumi.visualization import prepare, render, input_rows
class Params:
    def __init__(self, **values): self.values = dict(X='x', Y='y', Series='', Width=4, Height=3, Dpi=80, Theme='light', Title='Plot', XLabel='', YLabel='', MaxPoints=100, **{}) | values
    def __getattr__(self, name): return lambda: self.values[name]
class Context:
    run_id = '12345678-1234-1234-1234-123456789abc'
    card = 'chart'
    invocation = 1
    def __init__(self): self.p = Params(); self.output_directory = sys.argv[1]; self.sent = []; self.stop = False; self.deadline = 60000
    def params(self): return self.p
    def cancelled(self): return self.stop
    def remaining_ms(self): return self.deadline
    def display_figure(self, figure):
        self.sent.append(figure)
        tsugumi.Context.display_figure(self, figure)
    builder = tsugumi.Context.builder
    def output(self, port, builder, root): builder.Finish(root); self.output_bytes = bytes(builder.Output())
ctx = Context()
'''
    env = {**os.environ, 'PYTHONPATH': os.pathsep.join([str(tmp_path), str(ROOT / 'sdk/python')]),
           'MPLCONFIGDIR': str(tmp_path / 'mpl'), 'PYTHONDONTWRITEBYTECODE': '1'}
    script = tmp_path / 'run.py'
    script.write_text(prelude + '\n' + textwrap.dedent(code))
    return subprocess.run([sys.executable, str(script), str(tmp_path)], env=env, text=True, capture_output=True, timeout=90)


def bindings(tmp_path):
    schema = tmp_path / '_outputs.fbs'
    schema.write_text('namespace outputs.figure.visualization; table Files { value:string; } root_type Files;')
    subprocess.run([os.environ.get('TSUGUMI_FLATC', 'flatc'), '--python', '--gen-object-api', '-o', str(tmp_path), str(schema)], check=True)


def test_missing_nonfinite_sampling_and_cancellation(tmp_path):
    result = run(tmp_path, '''
rows = [{'x': x, 'y': x * 2} for x in range(10)] + [{'x': None, 'y': 2}, {'x': float('inf'), 'y': 3}, {'x': 1, 'y': 'nan'}]
data, counts = prepare(rows, 'x', 'y', '', 'line', 3)
assert [r['x'] for r in data] == [0, 4, 9]
assert counts == dict(source_rows=13, finite_rows=10, plotted_rows=3, skipped_rows=3)
assert prepare([], 'x', 'y', '', 'scatter', 2)[0] == []
large, stats = prepare([{'x': i, 'y': i} for i in range(100000)], 'x', 'y', '', 'scatter', 1000)
assert len(large) == 1000 and large[-1]['x'] == 99999 and stats['finite_rows'] == 100000
for args in [([{'a': 1}], 'x', 'y', '', 'line', 3), ([], 'x', 'y', '', 'line', 0)]:
    try: prepare(*args)
    except tsugumi.Failure: pass
    else: raise AssertionError('invalid input accepted')
ctx.stop = True
try: render(ctx, rows)
except tsugumi.Failure as e: assert e.kind == 'cancelled'
else: raise AssertionError('cancel ignored')
ctx.stop = False; ctx.deadline = 0
try: render(ctx, rows)
except tsugumi.Failure as e: assert e.kind == 'timeout'
else: raise AssertionError('deadline ignored')
''')
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize('library', ['matplotlib', 'plotly', 'altair'])
@pytest.mark.parametrize('kind', ['line', 'scatter', 'histogram'])
def test_exports_and_same_size_updates(tmp_path, library, kind):
    bindings(tmp_path)
    result = run(tmp_path, f'''
import matplotlib.pyplot as plt
from outputs.figure.visualization.Files import Files
rows = [{{'x': x, 'y': x * x, 'group': 'a' if x % 2 else 'b'}} for x in range(8)]
ctx.p = Params(Series='group', MaxPoints=5)
render(ctx, rows, {library!r}, {kind!r})
folder = Path(Files.GetRootAs(ctx.output_bytes, 0).Value().decode())
assert folder.name == 'figure'
assert (folder / 'source.csv').read_text().count('\\n') == 9
assert (folder / 'plotted.csv').read_text().count('\\n') == 6
assert json.loads((folder / 'metadata.json').read_text())['plotted_rows'] == 5
artifact = folder / ('plot.png' if {library!r} == 'matplotlib' else 'plot.json')
before = artifact.read_bytes()
ctx.invocation = 2
ctx.p = Params(Title='Updated same-size result', Theme='dark', MaxPoints=5)
render(ctx, [dict(r, y=r['y'] + 7) for r in rows], {library!r}, {kind!r})
assert artifact.read_bytes() != before
assert json.loads((folder / 'metadata.json').read_text())['invocation'] == 2
assert plt.get_fignums() == []
if {library!r} == 'matplotlib':
    assert (folder / 'plot.svg').is_file() and (folder / 'plot.pdf').read_bytes().startswith(b'%PDF')
    from PIL import Image
    assert Image.open(artifact).size == (320, 240)
else:
    html = (folder / 'plot.html').read_text()
    assert '<script src=' not in html and 'cdn.jsdelivr.net' not in html.split('<body>')[0]
''')
    assert result.returncode == 0, result.stderr


def test_shared_templates_are_current():
    spec = importlib.util.spec_from_file_location('visualization_templates', ROOT / 'ui/templates/visualization.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    for path, value in module.outputs().items():
        assert path.read_text() == value
    assert len(module.templates()) == 9


def test_typed_columns_csv_file_and_directory_inputs(tmp_path):
    schema = tmp_path / 'data.fbs'
    schema.write_text('namespace inputs.data.example; table Columns { x:[double]; y:[double]; } table File { value:string; } root_type Columns;')
    subprocess.run([os.environ.get('TSUGUMI_FLATC', 'flatc'), '--python', '--gen-object-api', '-o', str(tmp_path), str(schema)], check=True)
    result = run(tmp_path, '''
import flatbuffers
import tsugumi.visualization as visualization
from inputs.data.example.Columns import ColumnsT
from inputs.data.example.File import FileT
visualization.__file__ = str(Path(ctx.output_directory) / 'tsugumi/visualization.py')
contract = Path(ctx.output_directory) / '_contract.json'
def use(value, name, kind):
    b = flatbuffers.Builder(128); b.Finish(value.Pack(b)); raw = bytes(b.Output())
    ctx.input = lambda port, cls: cls.GetRootAs(raw, 0)
    contract.write_text(json.dumps({'inputs': [{'name': 'data', 'type': 'inputs.data.example.' + name, 'kind': kind}]}))
columns = ColumnsT(); columns.x = [1., 2.]; columns.y = [3., 4.]
use(columns, 'Columns', 'flatbuffer')
assert input_rows(ctx) == [{'x': 1., 'y': 3.}, {'x': 2., 'y': 4.}]
source = Path(ctx.output_directory) / 'data.csv'
source.write_text('x,y\\n1,3\\n2,4\\n')
file = FileT(); file.value = str(source)
use(file, 'File', 'file')
assert input_rows(ctx)[1] == {'x': '2', 'y': '4'}
file.value = ctx.output_directory
use(file, 'File', 'directory')
assert input_rows(ctx, member='data.csv')[0]['y'] == '3'
try: input_rows(ctx, member='../outside.csv')
except tsugumi.Failure: pass
else: raise AssertionError('directory traversal accepted')
''')
    assert result.returncode == 0, result.stderr


def test_empty_figures_and_failed_export_cleanup(tmp_path):
    bindings(tmp_path)
    result = run(tmp_path, '''
import matplotlib.pyplot as plt
from matplotlib.figure import Figure
for library in ['matplotlib', 'plotly', 'altair']:
    render(ctx, [], library, 'line')
    assert json.loads((Path(ctx.output_directory) / 'figure/metadata.json').read_text())['plotted_rows'] == 0
original = Figure.savefig
def fail_export(*args, **kwargs): raise OSError('test write failure')
Figure.savefig = fail_export
try:
    try: render(ctx, [{'x': 1, 'y': 2}], 'matplotlib', 'scatter')
    except OSError: pass
    else: raise AssertionError('failed export was hidden')
    assert plt.get_fignums() == []
finally:
    Figure.savefig = original
''')
    assert result.returncode == 0, result.stderr
