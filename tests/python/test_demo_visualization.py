"""The Visualization demo and templates run by a real core and worker.

test_template_card_edit_save_reload_run runs in every CI job (one Matplotlib
Card); the whole demo and the interactive templates are opt-in
(TSUGUMI_TEST_DEMO_RUNS=1), as the other Nix-heavy demo runs are.
"""
import csv
import io
import json
import os

import pytest
from conftest import ROOT, demo_environment
from projects import start_run, wait_run
from test_demos import connect, edit_card, set_param, stderr
from tsugumi_client import change, new_id, path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.RunState import RunState

opt_in = pytest.mark.skipif(os.environ.get('TSUGUMI_TEST_DEMO_RUNS') != '1', reason='Runs Nix-backed Visualization Cards')
TEMPLATES = ROOT / 'ui/templates/visualization.json'
VALIDATION_FAILED = 17
EDIT = ('library="matplotlib", kind=kind', 'library="matplotlib", edited="through a project save", kind=kind')


def only_line(root, files=None):
    """The demo cut to start -> aggregate -> line (24 typed rows into one figure);
    `files` replaces the line Card's definition with a template's files."""
    project_file = root / 'tsugumi.json'
    project = json.loads(project_file.read_text())
    project['canvas']['cards'] = [{'id': name} for name in ['start', 'aggregate', 'line']]
    project['canvas']['connections'] = [
        {'from': {'card': 'start', 'port': 'out'}, 'to': {'card': 'aggregate', 'port': 'start'}},
        {'from': {'card': 'aggregate', 'port': 'data'}, 'to': {'card': 'line', 'port': 'data'}},
    ]
    project_file.write_text(json.dumps(project))
    for name, content in (files or {}).items():
        (root / 'definitions/line' / name).write_text(content)


def figure(c, run_id, card='line'):
    """The Card's retained figure artifact and its metadata.json and plotted.csv."""
    art = next(a for a in c.call('ArtifactListRequest', runId=run_id, card=path(card)).artifacts if text(a.output) == 'figure')
    read = lambda member: bytes(c.call('ArtifactReadRequest', artifactId=text(art.artifactId), member=member).bytes).decode()
    return art, json.loads(read('metadata.json')), list(csv.DictReader(io.StringIO(read('plotted.csv'))))


def assert_belongs(art, meta, run_id, card='line'):
    """metadata.json names the run, Card and invocation the artifact was retained for."""
    assert text(art.runId) == meta['run_id'] == run_id
    assert meta['card'].split('/')[-1] == card
    assert art.invocation == meta['invocation'] and meta['invocation'] >= 1


def ran(c, run_id, card='line'):
    st = wait_run(c, run_id, timeout=1800)
    assert st.state == RunState.succeeded, (text(st.failure.diagnostic) if st.failure else '', stderr(c, run_id, card))
    return st


def save_source(c, member, fn):
    p = f'definitions/line/{member}'
    snap = c.call('ProjectInspectRequest')
    src = next(s for s in snap.sources if text(s.path) == p)
    body = fn(bytes(c.call('ProjectFileReadRequest', path=p, atRev=snap.rev).bytes).decode())
    return c.call('ProjectSaveRequest', baseRev=snap.rev, opId=new_id(), origin='test',
                  changes=[change('PutSource', path=p, bytes=list(body.encode()), baseHash=text(src.hash))])


def test_template_card_edit_save_reload_run(tmp_path):
    """The Matplotlib template, saved into a project and run by tsugumid: its
    defaults draw every row, a lower max_points fails instead of thinning,
    sample_every thins on purpose, an edit of its own source is saved and run,
    and after a restart each run's retained figure still says which
    invocation it came from."""
    files = json.loads(TEMPLATES.read_text())['visualization_matplotlib']
    env = demo_environment(tmp_path, 'visualization', edit=lambda root: only_line(root, files), language='en')
    try:
        c = connect(env)
        first = start_run(c, ['start'], propagate=True)
        ran(c, first)
        art, meta, plotted = figure(c, first)
        assert_belongs(art, meta, first)
        assert {'plot.png', 'plot.svg', 'plot.pdf', 'plotted.csv', 'metadata.json'} <= {text(m.path) for m in art.members}
        assert (meta['library'], meta['kind'], meta['source_rows'], meta['drawn_rows'], meta['sampled']) == ('matplotlib', 'line', 24, 24, False)
        assert [float(r['x']) for r in plotted] == [float(i) for i in range(24)]
        record = c.call('RunRecordRequest', runId=first, card=path('line')).record
        assert record.display and record.display.invocation == art.invocation

        # Fewer max_points than points: the Card stops and says what to change.
        edit_card(env, c, 'line', lambda card: set_param(card, 'max_points', 10))
        failed = start_run(c, ['aggregate'], propagate=True)
        assert wait_run(c, failed, timeout=1800).state == RunState.failed
        status = c.call('RunRecordRequest', runId=failed, card=path('line')).status
        assert status.state == CardState.failed and status.failure.kind == VALIDATION_FAILED
        said = text(status.failure.diagnostic) + stderr(c, failed, 'line')
        assert '24 points to draw exceed max_points (10)' in said and 'sample_every' in said

        # Sampling on purpose, and an edit of the Card's own source, both saved.
        edit_card(env, c, 'line', lambda card: set_param(card, 'sample_every', 3))
        save_source(c, 'src/main.py', lambda source: source.replace(*EDIT) if EDIT[0] in source else pytest.fail('template source changed'))
        sampled = start_run(c, ['aggregate'], propagate=True)
        ran(c, sampled)
        art, meta, plotted = figure(c, sampled)
        assert_belongs(art, meta, sampled)
        assert (meta['drawn_rows'], meta['sampled'], meta['sample_every'], meta['max_points']) == (8, True, 3, 10)
        assert 'SAMPLED: 1 point in 3 drawn (8 of 24)' in meta['note'] and meta['edited'] == 'through a project save'
        assert [float(r['x']) for r in plotted] == [float(i) for i in range(0, 24, 3)]

        # Reload: the saved project, its settings and the retained figures come back.
        env.stop()
        env.start()
        c = connect(env)
        card = json.loads(text(next(d for d in c.call('ProjectInspectRequest').definitions if text(d.id) == 'line').cardJson))
        assert {p['name']: p['data'] for p in card['params']}['sample_every'] == 3
        reloaded = start_run(c, ['start'], propagate=True)
        ran(c, reloaded)
        for run_id, drawn in [(first, 24), (sampled, 8), (reloaded, 8)]:
            art, meta, plotted = figure(c, run_id)
            assert_belongs(art, meta, run_id)
            assert meta['drawn_rows'] == len(plotted) == drawn
        assert meta['edited'] == 'through a project save'
    finally:
        env.cleanup()


@opt_in
def test_typed_aggregate_to_three_figures_to_report(tmp_path):
    env = demo_environment(tmp_path, 'visualization')
    try:
        c = connect(env)
        run = start_run(c, ['start'], propagate=True)
        state = wait_run(c, run, timeout=600)
        assert state.state == RunState.succeeded, (text(state.failure.diagnostic) if state.failure else '', [stderr(c, run, name) for name in ['aggregate', 'line', 'scatter', 'histogram', 'report']])
        for name in ['line', 'scatter', 'histogram']:
            result = c.call('RunRecordRequest', runId=run, card=path(name))
            assert result.record and result.record.display
            log = stderr(c, run, name)
            assert 'Glyph' not in log or 'missing from font' not in log
            art, meta, _ = figure(c, run, name)
            assert_belongs(art, meta, run, name)
            assert (meta['kind'], meta['drawn_rows'], meta['sampled']) == (name, 24, False)
        artifacts = c.call('ArtifactListRequest', runId=run)
        report = next(a for a in artifacts.artifacts if text(a.output) == 'report')
        members = {text(m.path) for m in report.members}
        assert 'index.html' in members
        for name in ['line', 'scatter', 'histogram']:
            assert {f'{name}/plot.png', f'{name}/plot.svg', f'{name}/plot.pdf', f'{name}/plotted.csv', f'{name}/metadata.json'} <= members
        before = c.call('RunRecordRequest', runId=run, card=path('line')).record.display
        edit_card(env, c, 'aggregate', lambda card: set_param(card, 'offset', 7))
        updated = start_run(c, ['aggregate'], propagate=True)
        assert wait_run(c, updated, timeout=600).state == RunState.succeeded
        after = c.call('RunRecordRequest', runId=updated, card=path('line')).record.display
        assert bytes(after.value.data) != bytes(before.value.data)
        restored = c.call('RunRecordRequest', runId=run, card=path('line')).record.display
        assert bytes(restored.value.data) == bytes(before.value.data)
        if hasattr(env, 'start'):
            env.stop()
            env.start()
            c = connect(env)
            persisted = c.call('RunRecordRequest', runId=run, card=path('line')).record.display
            assert bytes(persisted.value.data) == bytes(before.value.data)
            retained = c.call('ArtifactListRequest', runId=run).artifacts
            assert any(text(a.output) == 'report' for a in retained)
    finally:
        env.cleanup()


@opt_in
@pytest.mark.parametrize('library', ['plotly', 'altair'])
def test_interactive_template_keeps_offline_html_and_japanese_font(tmp_path, library):
    from harness.observations import read_pages
    files = json.loads(TEMPLATES.read_text())[f'visualization_{library}']

    def edit(root):
        only_line(root, files)
        card_file = root / 'definitions/line/_card.json'
        card = json.loads(card_file.read_text())
        set_param(card, 'title', '測定値の集計')
        card_file.write_text(json.dumps(card))

    env = demo_environment(tmp_path, 'visualization', edit=edit)
    try:
        c = connect(env)
        rid = start_run(c, ['start'], propagate=True)
        ran(c, rid)
        record = c.call('RunRecordRequest', runId=rid, card=path('line')).record
        assert record.display
        chart = json.loads(text(record.display.value.dataJson))
        assert '測定値の集計' in json.dumps(chart, ensure_ascii=False)
        art, meta, _ = figure(c, rid)
        assert_belongs(art, meta, rid)
        assert meta['library'] == library
        html = read_pages(c, 'ArtifactReadRequest', page_bytes=256 * 1024, artifactId=text(art.artifactId), member='plot.html').decode()
        assert '<script src=' not in html
        assert 'data:font/ttf;base64,' in html
        assert {'plot.json', 'plot.html', 'plotted.csv', 'metadata.json'} <= {text(m.path) for m in art.members}
    finally:
        env.cleanup()
