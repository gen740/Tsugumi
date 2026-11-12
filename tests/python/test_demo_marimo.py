"""Opt-in marimo scaffolds running through the real daemon and worker."""
import json
import os

import pytest
from conftest import demo_environment
from projects import start_run, wait_run
from test_demos import connect, edit_card, set_param, stderr
from harness.observations import read_pages
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get('TSUGUMI_TEST_DEMO_RUNS') != '1', reason='Runs Nix-backed notebook Cards')


def test_notebook_passes_typed_result_and_keeps_snapshot(tmp_path):
    env = demo_environment(tmp_path, 'marimo')
    try:
        c = connect(env)
        rid = start_run(c, ['start'], propagate=True)
        state = wait_run(c, rid, timeout=600)
        assert state.state == RunState.succeeded, stderr(c, rid, 'notebook')
        record = c.call('RunRecordRequest', runId=rid, card=path('notebook')).record
        assert record and '14' in text(record.display.value.text)
        downstream = c.call('RunRecordRequest', runId=rid, card=path('result')).record
        assert downstream and downstream.summary.invocation == 1
        art = next(a for a in c.call('ArtifactListRequest', runId=rid).artifacts if text(a.output) == 'report')
        def read(member):
            return read_pages(c, 'ArtifactReadRequest', artifactId=text(art.artifactId), member=member, page_bytes=256*1024)
        html = read('index.html')
        assert b'The selected result is' in html and b'data:image/png;base64,' in html
        assert json.loads(read('result.json')) == {'result':14}
        assert {'source/notebook.py','assets/cell-2.png','metadata.json','bindings.json'} <= {text(m.path) for m in art.members}
        edit_card(env,c,'notebook',lambda card:set_param(card,'factor',3))
        updated=start_run(c,['notebook'],propagate=True)
        assert wait_run(c,updated,timeout=600).state == RunState.succeeded, stderr(c,updated,'notebook')
        assert '21' in text(c.call('RunRecordRequest',runId=updated,card=path('notebook')).record.display.value.text)
        assert read('index.html') == html
        if hasattr(env,'start'):
            env.stop();env.start();c=connect(env)
            assert read('index.html') == html
    finally:
        env.cleanup()


@pytest.mark.parametrize('ending',['failure','cancel','deadline','project_stop'])
def test_project_owns_notebook_process_tree(tmp_path,ending):
    if os.environ.get('TSUGUMI_TEST_CONTAINER')=='1':
        pytest.skip('Host process identity assertion; VM lifecycle has separate core tests')
    import re
    import time
    def edit(root):
        card_file=root/'definitions/notebook/_card.json'
        card=json.loads(card_file.read_text())
        if ending=='deadline':card['timeoutSeconds']=12
        card_file.write_text(json.dumps(card))
        (root/'definitions/notebook/src/notebook.py').write_text('''import os,sys,subprocess,time
child=subprocess.Popen([sys.executable,"-c","import time; time.sleep(120)"])
print("NOTEBOOK_PROCESSES",os.getpid(),child.pid,flush=True)
''' + ('raise RuntimeError("intentional cell failure")\n' if ending=='failure' else 'time.sleep(120)\n'))
    env=demo_environment(tmp_path,'marimo',edit=edit)
    try:
        c=connect(env);rid=start_run(c,['start'],propagate=True)
        until=time.monotonic()+60;match=None
        while time.monotonic()<until:
            log=text(c.call('LogRangeRequest',runId=rid,card=path('notebook'),stream=1,limitBytes=262144).text)
            match=re.search(r'NOTEBOOK_PROCESSES (\d+) (\d+)',log)
            if match:break
            time.sleep(.05)
        assert match,'Notebook did not report process identity'
        pids=[int(x) for x in match.groups()]
        if ending=='cancel':c.call('RunCancelRequest',runId=rid)
        if ending=='project_stop':env.stop()
        else:
            state=wait_run(c,rid,timeout=30)
            assert state.state!=RunState.succeeded
        def alive(pid):
            try:os.kill(pid,0);return True
            except ProcessLookupError:return False
        until=time.monotonic()+5
        while time.monotonic()<until and any(alive(pid) for pid in pids):time.sleep(.05)
        assert not any(alive(pid) for pid in pids),f'Notebook process survived {ending}: {pids}'
    finally:env.cleanup()


def test_two_cards_run_the_same_notebook_without_shared_state(tmp_path):
    import shutil
    def edit(root):
        shutil.copytree(root/'definitions/notebook',root/'definitions/notebook_b')
        other=root/'definitions/notebook_b/_card.json'
        card=json.loads(other.read_text());set_param(card,'factor',5);other.write_text(json.dumps(card))
        fork=root/'definitions/share';fork.mkdir()
        (fork/'_card.json').write_text(json.dumps({'template':'fork','params':[{'name':'branches','type':'string[]','data':['a','b']}]}))
        project_file=root/'tsugumi.json';project=json.loads(project_file.read_text())
        project['canvas']['cards'] += [{'id':'share'},{'id':'notebook_b'}]
        wires=project['canvas']['connections']
        wires[:]=[w for w in wires if w['to']['card']!='notebook']
        for a,ap,b,bp in [('source','value','share','in'),('share','a','notebook','data'),('share','b','notebook_b','data')]:
            wires.append({'from':{'card':a,'port':ap},'to':{'card':b,'port':bp}})
        project_file.write_text(json.dumps(project))
    env=demo_environment(tmp_path,'marimo',edit=edit)
    try:
        c=connect(env);rid=start_run(c,['start'],propagate=True)
        assert wait_run(c,rid,timeout=600).state==RunState.succeeded,stderr(c,rid,'notebook_b')
        artifacts=c.call('ArtifactListRequest',runId=rid).artifacts
        for name,value in [('notebook',14),('notebook_b',35)]:
            art=next(a for a in artifacts if text(a.card.segments[0])==name)
            def read(member):return read_pages(c,'ArtifactReadRequest',artifactId=text(art.artifactId),member=member,page_bytes=256*1024)
            assert json.loads(read('result.json'))=={'result':value}
            assert json.loads(read('metadata.json'))['card']==name
    finally:env.cleanup()
