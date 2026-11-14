"""Run a marimo notebook once per invocation and keep a static result snapshot.

The child entry point is deliberately standalone: it never imports the embedded
worker host, and cannot emit a Card output during reactive cell execution.
"""
from __future__ import annotations

import ast
import html
import json
import keyword
import os
from pathlib import Path
import shutil
import subprocess
import sys
import traceback


def _plain(value):
    if isinstance(value, bytes):
        return value.decode('utf-8')
    if isinstance(value, (list, tuple)):
        return [_plain(v) for v in value]
    if hasattr(value, 'tolist'):
        return _plain(value.tolist())
    if hasattr(value, '__dict__'):
        return {k: _plain(v) for k, v in vars(value).items()}
    return value


def bindings(inputs, mapping):
    """Resolve notebook variable -> inputs.<port>.<field> or param.<field>."""
    from tsugumi import Failure, contract
    if not isinstance(mapping, dict):
        raise Failure('validation_failed', 'bindings must be a JSON object mapping variable names to inputs/param paths')
    values = {
        'inputs': {port['name']: _plain(contract.input_object(inputs, port['name'])) for port in contract.ports()},
        'param': _plain(contract.params_object(inputs)),
    }
    result = {}
    for name, path in mapping.items():
        if not name.isidentifier() or keyword.iskeyword(name) or not isinstance(path, str):
            raise Failure('validation_failed', 'Bindings must map Python variable names to inputs/param paths')
        value = values
        try:
            for field in path.split('.'):
                value = value[field]
        except (KeyError, TypeError):
            raise Failure('validation_failed', f'Notebook binding {name!r} has no source {path!r}') from None
        result[name] = value
    return result


def run(report, run, source_directory, notebook, definitions, selected):
    """Return (selected JSON-compatible definitions, retained Directory path).

    The surrounding Python Card packs its typed outputs only after this returns.
    No marimo server or browser is started. Child processes stay in the worker's
    process group, which the scheduler owns on failure/cancel/deadline/stop.
    """
    from tsugumi import Failure
    root = Path(source_directory).resolve()
    source = (root / notebook).resolve()
    if not source.is_relative_to(root) or source.suffix != '.py' or not source.is_file():
        raise Failure('validation_failed', 'notebook must name a .py file inside the Card source directory')
    # The sandbox exposes the invocation's output directory to the worker
    # through a descriptor. A subprocess does not inherit that directory.
    work = Path(run.temporary_directory) / 'marimo'
    snapshot = work / 'notebook'
    snapshot.mkdir(parents=True)
    shutil.copytree(root, snapshot / 'source', ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    config = {
        'notebook': str(snapshot / 'source' / source.relative_to(root)),
        'directory': str(snapshot), 'definitions': definitions, 'selected': selected,
        'source': notebook, 'run_id': run.run_id, 'card': run.card, 'invocation': run.invocation,
        'fonts': str(Path(__file__).with_name('_fonts.txt')),
    }
    request = work / 'request.json'
    request.write_text(json.dumps(config, ensure_ascii=False, allow_nan=False))
    environment = {**os.environ, 'PYTHONPATH': os.pathsep.join(sys.path), 'MPLBACKEND': 'Agg',
                   'MPLCONFIGDIR': str(work / 'matplotlib'), 'XDG_CONFIG_HOME': str(work / 'config'),
                   'PYTHONDONTWRITEBYTECODE': '1'}
    python = str(Path(sys.prefix) / 'bin/python3')
    child = subprocess.Popen([python, str(Path(__file__).resolve()), str(request)],
                             cwd=snapshot / 'source', env=environment)
    try:
        while child.poll() is None:
            if run.cancelled():
                raise Failure('cancelled', 'Notebook invocation cancelled')
            if run.remaining_ms() == 0:
                raise Failure('timeout', 'Notebook invocation deadline reached')
            try:
                child.wait(timeout=0.05)
            except subprocess.TimeoutExpired:
                pass
        if child.returncode:
            diagnostic = snapshot / 'failure.json'
            detail = json.loads(diagnostic.read_text())['message'] if diagnostic.exists() else f'Notebook process exited with status {child.returncode}'
            raise Failure('worker_failed', detail)
        if run.cancelled():
            raise Failure('cancelled', 'Notebook invocation cancelled')
        if run.remaining_ms() == 0:
            raise Failure('timeout', 'Notebook invocation deadline reached')
        result = json.loads((snapshot / 'result.json').read_text())
        folder = Path(run.output_directory) / 'notebook'
        shutil.copytree(snapshot, folder)
        report.markdown(f"**{notebook}** — committed notebook result\n\n" + '\n'.join(f'- `{k}`: {v}' for k, v in result.items()))
        return result, folder
    finally:
        if child.poll() is None:
            child.terminate()
            try:
                child.wait(timeout=0.3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()


def _fragment(value, assets, index):
    """Portable cell outputs; frontend-only widgets are explicitly static text."""
    if value is None:
        return ''
    if hasattr(value, 'savefig'):
        import io
        import base64
        png = io.BytesIO()
        value.savefig(png, format='png', dpi=120)
        data = png.getvalue()
        (assets / f'cell-{index}.png').write_bytes(data)
        return '<img alt="Cell figure" src="data:image/png;base64,' + base64.b64encode(data).decode() + '">'
    if hasattr(value, '_repr_html_'):
        markup = value._repr_html_()
        # Standard Markdown/HTML and data images work without marimo's frontend.
        # Custom elements need that frontend; a static artifact must not imply it
        # supports reactive controls or send their values to downstream Cards.
        import re
        if markup and not re.search(r'<(?:marimo-|[\w]+-[\w]+)', markup):
            return markup
        return '<p>Interactive cell output is not included in the static preview. See the saved notebook source.</p>'
    return '<pre>' + html.escape(str(value)) + '</pre>'


def _execute(request):
    import runpy
    config = json.loads(Path(request).read_text())
    folder = Path(config['directory'])
    notebook = Path(config['notebook'])
    code = notebook.read_text()
    cells = [{'name': n.name, 'line': n.lineno, 'code': ast.get_source_segment(code, n)}
             for n in ast.parse(code).body if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
    print('Notebook cells: ' + ', '.join(f"{c['name']} ({config['source']}:{c['line']})" for c in cells), flush=True)
    try:
        # Matplotlib may be imported by marimo formatters. Configure the same
        # pinned font before executing user cells, without the worker backend.
        fonts = Path(config['fonts'])
        if fonts.exists() and fonts.read_text().strip():
            import matplotlib
            from matplotlib import font_manager
            families = []
            for file in fonts.read_text().splitlines():
                font_manager.fontManager.addfont(file)
                families.append(font_manager.FontProperties(fname=file).get_name())
            matplotlib.rcParams['font.family'] = ['DejaVu Sans'] + families
        sys.path.insert(0, str(notebook.parent))
        app = runpy.run_path(str(notebook))['app']
        outputs, definitions = app.run(defs=config['definitions'])
        result = {name: _plain(definitions[name]) for name in config['selected']}
        encoded = json.dumps(result, ensure_ascii=False, allow_nan=False)
        assets = folder / 'assets'
        assets.mkdir()
        sections = ['<section>' + _fragment(value, assets, i) + '</section>' for i, value in enumerate(outputs)]
        source = ''.join(f'<details><summary>{html.escape(c["name"])} · {html.escape(config["source"])}:{c["line"]}</summary><pre>{html.escape(c["code"] or "")}</pre></details>' for c in cells)
        csp = "default-src 'none'; img-src data:; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'"
        page = '<!doctype html><html><head><meta charset="utf-8"><meta http-equiv="Content-Security-Policy" content="' + csp + '"><title>Notebook result</title><style>body{font:16px sans-serif;max-width:1000px;margin:32px auto;padding:0 20px}img{max-width:100%}pre{white-space:pre-wrap}section{margin:20px 0}</style></head><body>'
        page += '<h1>' + html.escape(config['source']) + '</h1><p>Saved result of one Tsugumi invocation. Controls are static; editing this preview does not change a Run.</p>'
        page += ''.join(sections) + '<h2>Selected results</h2><pre>' + html.escape(encoded) + '</pre><h2>Source cells</h2>' + source + '</body></html>'
        (folder / 'index.html').write_text(page)
        (folder / 'result.json').write_text(encoded)
        (folder / 'bindings.json').write_text(json.dumps(config['definitions'], ensure_ascii=False))
        (folder / 'metadata.json').write_text(json.dumps({k: config[k] for k in ['source', 'run_id', 'card', 'invocation']}, indent=2))
    except BaseException as error:
        message = f"{config['source']}: {type(error).__name__}: {error} (see cell names and traceback in stderr)"
        (folder / 'failure.json').write_text(json.dumps({'message': message, 'cells': cells}))
        traceback.print_exc()
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(_execute(sys.argv[1]))
