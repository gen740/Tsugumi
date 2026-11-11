"""Editable Visualization Card scaffolds: typed/CSV/JSON input, figures and artifacts.

The UI writes a normal Python Card. The library and shape are chosen in its
source; data and appearance are ordinary params, with no hidden view state.
"""
from __future__ import annotations

import base64
import csv
import importlib
import json
import math
from pathlib import Path

MAX_ROWS = 1_000_000
MAX_FILE_BYTES = 64 * 1024 * 1024


def _fail(message):
    from tsugumi import Failure
    raise Failure("validation_failed", message)


def _check(ctx):
    from tsugumi import Failure
    if ctx.cancelled():
        raise Failure("cancelled", "Visualization cancelled")
    if ctx.remaining_ms() == 0:
        raise Failure("timeout", "Visualization deadline reached")


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


def input_rows(ctx, field='', member='data.csv'):
    """Read the connected data port using its generated type, never a guessed schema."""
    contract = json.loads((Path(__file__).parent.parent / '_contract.json').read_text())
    port = next((p for p in contract['inputs'] if p['name'] == 'data'), None)
    if port is None:
        _fail('Connect an upstream output to data before running this Visualization Card')
    module = importlib.import_module(port['type'])
    name = port['type'].split('.')[-1]
    root = ctx.input('data', getattr(module, name))
    value = _plain(getattr(module, name + 'T').InitFromObj(root))
    if port['kind'] != 'flatbuffer':
        source = Path(value['value'])
        if port['kind'] == 'directory':
            candidate = (source / member).resolve()
            if not member or Path(member).is_absolute() or not candidate.is_relative_to(source.resolve()):
                _fail('file must be a relative member of the input directory')
            source = candidate
        if source.stat().st_size > MAX_FILE_BYTES:
            _fail('Input file exceeds 64 MiB; aggregate it upstream')
        if source.suffix.lower() == '.csv':
            with source.open(newline='', encoding='utf-8-sig') as f:
                rows = []
                for row in csv.DictReader(f):
                    if len(rows) >= MAX_ROWS:
                        _fail('Input exceeds 1,000,000 rows; aggregate it upstream')
                    if len(rows) % 1024 == 0:
                        _check(ctx)
                    rows.append(row)
            value = rows
        elif source.suffix.lower() == '.json':
            value = json.loads(source.read_text())
        else:
            _fail('File inputs must be UTF-8 CSV or JSON; edit input_rows for another format')
    for part in field.split('.') if field else []:
        if not isinstance(value, dict) or part not in value:
            _fail(f'data_path does not contain {part!r}')
        value = value[part]
    if isinstance(value, dict):
        columns = {k: v for k, v in value.items() if isinstance(v, list)}
        if not columns or any(len(v) > MAX_ROWS for v in columns.values()) or len({len(v) for v in columns.values()}) != 1:
            _fail('Choose data_path containing rows or equally sized column arrays')
        value = [dict(zip(columns, row)) for row in zip(*columns.values())]
    if not isinstance(value, list) or len(value) > MAX_ROWS or not all(isinstance(row, dict) for row in value):
        _fail('Data must be a row table or equally sized column arrays (at most 1,000,000 rows)')
    return value


def prepare(rows, x, y, series, kind, limit, check=lambda: None):
    """Filter missing/non-finite coordinates, then sample deterministically in input order."""
    if not 1 <= limit <= 20_000:
        _fail('max_points must be between 1 and 20,000')
    axes = [x] if kind == 'histogram' else [x, y]
    if rows and any(not any(axis in r for r in rows) for axis in axes):
        _fail('An axis column is missing; set x and y to columns in the input')
    if series and rows and not any(series in r for r in rows):
        _fail('The series column is missing')
    valid = []
    for i, row in enumerate(rows):
        if i % 1024 == 0:
            check()
        try:
            values = [float(row.get(axis)) for axis in axes]
        except (TypeError, ValueError):
            continue
        if not all(math.isfinite(v) for v in values):
            continue
        valid.append({'x': values[0], 'y': values[-1], 'series': str(row.get(series, '')) if series else ''})
    total = len(valid)
    if total > limit:
        # Include both endpoints; no random state is retained by a resident worker.
        valid = [valid[round(i * (total - 1) / (limit - 1))] for i in range(limit)] if limit > 1 else valid[:1]
    if len({r['series'] for r in valid}) > 100:
        _fail('More than 100 series; aggregate or choose another series column')
    return valid, {'source_rows': len(rows), 'finite_rows': total, 'plotted_rows': len(valid), 'skipped_rows': len(rows) - total}


def _csv(path, rows):
    columns = list(dict.fromkeys(k for row in rows for k in row))
    with path.open('w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def _font_css():
    # The pinned Japanese font travels with the standalone HTML too.
    fonts = Path(__file__).with_name('_fonts.txt')
    if fonts.exists():
        path = next((Path(p) for p in fonts.read_text().splitlines() if 'gothic' in p.lower() or 'ipaexg' in p.lower()), None)
        if path:
            data = base64.b64encode(path.read_bytes()).decode()
            return '<style>@font-face{font-family:TsugumiFigure;src:url(data:font/ttf;base64,' + data + ')}</style>'
    return ''


def render(ctx, rows, library='matplotlib', kind='line'):
    """Render and publish a normal Directory output; UI gestures never enter params."""
    if library not in ('matplotlib', 'plotly', 'altair') or kind not in ('line', 'scatter', 'histogram'):
        _fail('Unknown visualization library or shape')
    p = ctx.params()
    text = lambda v: v.decode() if isinstance(v, bytes) else (v or '')
    x, y, series = text(p.X()), text(p.Y()), text(p.Series())
    width, height, dpi = float(p.Width()), float(p.Height()), int(p.Dpi())
    if not (1 <= width <= 20 and 1 <= height <= 20 and 50 <= dpi <= 300):
        _fail('width/height must be 1–20 inches and dpi 50–300')
    theme = text(p.Theme())
    if theme not in ('light', 'dark'):
        _fail('theme must be light or dark')
    data, counts = prepare(rows, x, y, series, kind, int(p.MaxPoints()), lambda: _check(ctx))
    note = f"Showing {counts['plotted_rows']} / {counts['finite_rows']} finite rows; skipped {counts['skipped_rows']} missing/non-finite rows"
    title = text(p.Title()) or kind.title()
    xlabel, ylabel = text(p.XLabel()) or x, text(p.YLabel()) or ('count' if kind == 'histogram' else y)
    folder = Path(ctx.output_directory) / 'figure'
    folder.mkdir(parents=True, exist_ok=True)
    _csv(folder / 'source.csv', rows)
    _csv(folder / 'plotted.csv', data)
    (folder / 'metadata.json').write_text(json.dumps({**counts, 'run_id': ctx.run_id, 'card': ctx.card, 'invocation': ctx.invocation, 'library': library, 'kind': kind, 'note': note}, ensure_ascii=False, indent=2))
    _check(ctx)
    groups = {name: [r for r in data if r['series'] == name] for name in dict.fromkeys(r['series'] for r in data)}
    if library == 'matplotlib':
        import matplotlib.pyplot as plt
        fonts = plt.rcParams['font.family']
        with plt.style.context('dark_background' if theme == 'dark' else {}):
            plt.rcParams['font.family'] = fonts
            fig, ax = plt.subplots(figsize=(width, height), dpi=dpi)
            try:
                for name, values in groups.items():
                    xs, ys = [r['x'] for r in values], [r['y'] for r in values]
                    if kind == 'line': ax.plot(xs, ys, label=name)
                    elif kind == 'scatter': ax.scatter(xs, ys, label=name)
                    else: ax.hist(xs, bins='auto', label=name, alpha=0.7)
                ax.set(xlabel=xlabel, ylabel=ylabel, title=title)
                if series and groups: ax.legend()
                fig.text(0.02, 0.01, note, fontsize=8)
                if not data: ax.text(0.5, 0.5, 'No finite data', transform=ax.transAxes, ha='center')
                fig.tight_layout(rect=(0, 0.05, 1, 1))
                for ext in ('png', 'svg', 'pdf'):
                    _check(ctx)
                    fig.savefig(folder / ('plot.' + ext), dpi=dpi)
                ctx.display_figure(fig)
            finally:
                plt.close(fig)
    elif library == 'plotly':
        import plotly.graph_objects as go
        fig = go.Figure()
        for name, values in groups.items():
            xs, ys = [r['x'] for r in values], [r['y'] for r in values]
            fig.add_trace(go.Histogram(x=xs, name=name) if kind == 'histogram' else go.Scatter(x=xs, y=ys, name=name, mode='lines' if kind == 'line' else 'markers'))
        fig.update_layout(title=title, xaxis_title=xlabel, yaxis_title=ylabel, width=round(width * dpi), height=round(height * dpi), template='plotly_dark' if theme == 'dark' else 'plotly_white', font_family='TsugumiFigure, sans-serif', showlegend=bool(series))
        fig.add_annotation(text=note, x=0, y=-0.22, xref='paper', yref='paper', showarrow=False)
        if not data: fig.add_annotation(text='No finite data', x=0.5, y=0.5, xref='paper', yref='paper', showarrow=False)
        fig.write_json(folder / 'plot.json')
        html = fig.to_html(include_plotlyjs=True, full_html=True)
        (folder / 'plot.html').write_text(html.replace('<head>', '<head>' + _font_css()), encoding='utf-8')
        ctx.display_figure(fig)
    else:
        import altair as alt
        chart = alt.Chart(alt.Data(values=data)).properties(title=alt.TitleParams(text=title, subtitle=note), width=round(width * dpi), height=round(height * dpi))
        color = {'color': alt.Color('series:N', title=series)} if series else {}
        if kind == 'histogram': chart = chart.mark_bar().encode(x=alt.X('x:Q', bin=True, title=xlabel), y=alt.Y('count():Q', title=ylabel), **color)
        else:
            chart = (chart.mark_line() if kind == 'line' else chart.mark_point()).encode(x=alt.X('x:Q', title=xlabel), y=alt.Y('y:Q', title=ylabel), tooltip=['x:Q', 'y:Q', 'series:N'], **color).interactive()
        ink = 'white' if theme == 'dark' else '#222'
        chart = chart.configure(font='TsugumiFigure', background='#222' if theme == 'dark' else 'white').configure_axis(labelColor=ink, titleColor=ink).configure_legend(labelColor=ink, titleColor=ink).configure_title(color=ink, subtitleColor=ink)
        chart.save(folder / 'plot.json')
        chart.save(folder / 'plot.html', inline=True)
        html_path = folder / 'plot.html'
        html_path.write_text(html_path.read_text().replace('<head>', '<head>' + _font_css()), encoding='utf-8')
        ctx.display_figure(chart)
    _check(ctx)
    import outputs.figure.visualization.Files as output
    b = ctx.builder()
    value = b.CreateString(str(folder))
    output.Start(b); output.AddValue(b, value)
    ctx.output('figure', b, output.End(b))
    print(note)
