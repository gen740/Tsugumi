"""Opt-in marimo scaffolds running through the real daemon and worker."""

# TODO(Card ABI): the bundled demos are still on the previous Card ABI and are ported once the
# recording of displays is designed (docs/Demos.md, "TODO: port the demos to
# the current Card ABI"). Until then these demo runs are skipped, not ported.
import pytest

pytest.skip("TODO(Card ABI): demos are postponed until display recording is designed; "
            "see docs/Demos.md", allow_module_level=True)
import base64
import json
import re
import os

import pytest
from conftest import demo_environment
from harness.file_events import wait_for_exit
from projects import start_run, wait_for_log, wait_run
from test_demos import connect, edit_card, set_param, stderr
from harness.observations import read_pages
from tsugumi_client import path, text
from Tsugumi.Data.CardState import CardState
from Tsugumi.Data.RunState import RunState
from Tsugumi.Data.FailureKind import FailureKind

extra_demo_runs = pytest.mark.skipif(os.environ.get('TSUGUMI_TEST_DEMO_RUNS') != '1',
                                    reason='Runs additional notebook daemon cases')


def test_notebook_passes_typed_result_and_keeps_snapshot(tmp_path):
    env = demo_environment(tmp_path, 'marimo')
    try:
        c = connect(env)
        rid = start_run(c, ['start'], propagate=True)
        state = wait_run(c, rid)
        assert state.state == RunState.succeeded, stderr(c, rid, 'notebook')
        record = c.call('RunRecordRequest', runId=rid, card=path('notebook')).record
        downstream = c.call('RunRecordRequest', runId=rid, card=path('result')).record
        assert json.loads(text(downstream.display.value.text)) == {'value': 14}
        art = next(a for a in c.call('ArtifactListRequest', runId=rid).artifacts if text(a.output) == 'report')
        def read(member):
            return read_pages(c, 'ArtifactReadRequest', artifactId=text(art.artifactId), member=member, page_bytes=256*1024)
        html = read('index.html')
        assert json.loads(read('result.json')) == {'result':14}
        assert json.loads(read('bindings.json')) == {'source': 7, 'factor': 2}
        figure = re.search(rb'data:image/png;base64,([A-Za-z0-9+/=]+)', html)[1]
        assert base64.b64decode(figure).startswith(b'\x89PNG\r\n\x1a\n')
        assert json.loads(read('metadata.json')) == {
            'source': 'notebook.py', 'run_id': rid, 'card': 'notebook',
            'invocation': record.summary.invocation,
        }
        edit_card(env, c, 'notebook', lambda card: set_param(card, 'factor', 3))
        updated = start_run(c, ['source'], propagate=True)
        assert wait_run(c,updated).state == RunState.succeeded, stderr(c,updated,'notebook')
        downstream = c.call('RunRecordRequest', runId=updated, card=path('result')).record
        assert json.loads(text(downstream.display.value.text)) == {'value': 21}
        assert read('index.html') == html
    finally:
        env.cleanup()


@pytest.mark.parametrize("problem", ["binding", "cell"])
def test_a_failed_notebook_sends_no_downstream_result(tmp_path, problem):
    failed_line = []
    def edit(root):
        notebook = root / 'definitions/notebook'
        if problem == 'binding':
            settings = notebook / '_card.json'
            card = json.loads(settings.read_text())
            set_param(card, 'bindings', '{"source":"inputs.data.source","result":"inputs.data.unknown"}')
            settings.write_text(json.dumps(card))
        else:
            source = notebook / 'src/notebook.py'
            source.write_text(source.read_text().replace(
                'def explanation(result):', '''def explanation(result):
    raise RuntimeError("notebook test failure")'''
            ))
            failed_line.append(next(i for i, line in enumerate(source.read_text().splitlines(), 1)
                                    if 'raise RuntimeError' in line))

    env = demo_environment(tmp_path, 'marimo', edit=edit)
    try:
        client = connect(env)
        rid = start_run(client, ['start'], propagate=True)
        state = wait_run(client, rid)
        assert state.state == RunState.failed
        record = client.call('RunRecordRequest', runId=rid, card=path('notebook'))
        failure = record.status.failure
        assert failure.kind == (FailureKind.validation_failed if problem == 'binding' else FailureKind.worker_failed)
        if problem == 'cell':
            args = {text(a.key): text(a.value) for a in failure.args}
            assert args['source'] == 'src/notebook.py'
            assert args['cell'] == 'explanation'
            assert int(args['line']) == failed_line[0]
        downstream = client.call('RunInvocationsRequest', runId=rid, card=path('result'))
        blocked, = downstream.records
        assert blocked.state == CardState.blocked and blocked.startedAt is None
        assert not client.call('ArtifactListRequest', runId=rid).artifacts
    finally:
        env.cleanup()


@extra_demo_runs
@pytest.mark.parametrize('ending',['failure','cancel','project_stop'])
def test_project_owns_notebook_process_tree(tmp_path,ending):
    if os.environ.get('TSUGUMI_TEST_SHARED_VM')=='1':
        pytest.skip('Host process identity assertion; VM lifecycle has separate core tests')
    def edit(root):
        (root/'definitions/notebook/src/notebook.py').write_text('''import marimo
app = marimo.App()
@app.cell
def configuration():
    source = 0
    factor = 0
    return source, factor
@app.cell
def lifecycle():
    import os, sys, subprocess, time
    child=subprocess.Popen([sys.executable,"-c","import time; time.sleep(120)"])
    print("NOTEBOOK_PROCESSES",os.getpid(),child.pid,flush=True)
''' + ('''    raise RuntimeError("intentional cell failure")
''' if ending=='failure' else '''    time.sleep(120)
'''))
    env=demo_environment(tmp_path,'marimo',edit=edit)
    try:
        c=connect(env);rid=start_run(c,['start'],propagate=True)
        pattern = re.compile(r'NOTEBOOK_PROCESSES (\d+) (\d+)')
        log = wait_for_log(c, rid, 'notebook', 1, lambda log: pattern.search(text(log.text or b"")))
        pids = [int(pid) for pid in pattern.search(text(log.text or b"")).groups()]
        if ending=='cancel':c.call('RunCancelRequest',runId=rid)
        if ending=='project_stop':env.stop()
        else:
            expected = RunState.cancelled if ending == 'cancel' else RunState.failed
            assert wait_run(c, rid).state == expected
        for pid in pids:
            wait_for_exit(pid)
    finally:env.cleanup()


@extra_demo_runs
def test_two_cards_run_the_same_notebook_without_shared_state(tmp_path):
    import shutil
    def edit(root):
        shutil.copytree(root/'definitions/notebook',root/'definitions/notebook_b')
        other=root/'definitions/notebook_b/_card.json'
        card = json.loads(other.read_text())
        set_param(card, 'selected', 'source')
        other.write_text(json.dumps(card))
        project_file=root/'tsugumi.json';project=json.loads(project_file.read_text())
        project['canvas']['cards'] += [{'id':'notebook_b'}]
        wires=project['canvas']['connections']
        wires.append({'from':{'card':'source','port':'value'},'to':{'card':'notebook_b','port':'data'}})
        project_file.write_text(json.dumps(project))
    env=demo_environment(tmp_path,'marimo',edit=edit)
    try:
        c=connect(env);rid=start_run(c,['start'],propagate=True)
        assert wait_run(c,rid).state==RunState.succeeded,stderr(c,rid,'notebook_b')
        artifacts=c.call('ArtifactListRequest',runId=rid).artifacts
        for name,value in [('notebook',14),('notebook_b',7)]:
            art=next(a for a in artifacts if text(a.card.segments[0])==name)
            def read(member):return read_pages(c,'ArtifactReadRequest',artifactId=text(art.artifactId),member=member,page_bytes=256*1024)
            selected = 'result' if name == 'notebook' else 'source'
            assert json.loads(read('result.json')) == {selected: value}
    finally:env.cleanup()
