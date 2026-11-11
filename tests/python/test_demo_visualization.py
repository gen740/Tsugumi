"""Opt-in actual core/worker run of the shared Visualization scaffold demo."""
import json
import os

import pytest
from conftest import demo_environment
from projects import start_run, wait_run
from test_demos import connect, edit_card, set_param, stderr
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get('TSUGUMI_TEST_DEMO_RUNS') != '1', reason='Runs Nix-backed Visualization Cards')


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
        artifacts = c.call('ArtifactListRequest', runId=run)
        report = next(a for a in artifacts.artifacts if text(a.output) == 'report')
        members = {text(m.path) for m in report.members}
        assert 'index.html' in members
        for name in ['line', 'scatter', 'histogram']:
            assert {f'{name}/plot.png', f'{name}/plot.svg', f'{name}/plot.pdf', f'{name}/source.csv', f'{name}/metadata.json'} <= members
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


@pytest.mark.parametrize('library', ['plotly', 'altair'])
def test_interactive_scaffold_keeps_offline_html_and_japanese_font(tmp_path, library):
    from conftest import ROOT
    from harness.observations import read_pages
    files = json.loads((ROOT / 'ui/templates/visualization.json').read_text())[f'visualization_{library}_line']

    def edit(root):
        project_file = root / 'tsugumi.json'
        project = json.loads(project_file.read_text())
        project['canvas']['cards'] = [{'id': name} for name in ['start', 'aggregate', 'line']]
        project['canvas']['connections'] = [
            {'from': {'card': 'start', 'port': 'out'}, 'to': {'card': 'aggregate', 'port': 'start'}},
            {'from': {'card': 'aggregate', 'port': 'data'}, 'to': {'card': 'line', 'port': 'data'}},
        ]
        project_file.write_text(json.dumps(project))
        definition = root / 'definitions/line'
        for name, content in files.items():
            (definition / name).write_text(content)
        card_file = definition / '_card.json'
        card = json.loads(card_file.read_text())
        next(p for p in card['params'] if p['name'] == 'title')['data'] = '測定値の集計'
        card_file.write_text(json.dumps(card))

    env = demo_environment(tmp_path, 'visualization', edit=edit)
    try:
        c = connect(env)
        rid = start_run(c, ['start'], propagate=True)
        assert wait_run(c, rid, timeout=600).state == RunState.succeeded, stderr(c, rid, 'line')
        record = c.call('RunRecordRequest', runId=rid, card=path('line')).record
        assert record.display
        chart = json.loads(text(record.display.value.dataJson))
        assert '測定値の集計' in json.dumps(chart, ensure_ascii=False)
        art = next(a for a in c.call('ArtifactListRequest', runId=rid).artifacts if text(a.output) == 'figure')
        html = read_pages(c, 'ArtifactReadRequest', page_bytes=256 * 1024, artifactId=text(art.artifactId), member='plot.html').decode()
        assert '<script src=' not in html
        assert 'data:font/ttf;base64,' in html
        assert {'plot.json', 'plot.html', 'source.csv', 'metadata.json'} <= {text(m.path) for m in art.members}
    finally:
        env.cleanup()
