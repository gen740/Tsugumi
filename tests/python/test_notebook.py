"""Fresh notebook invocations without a UI or interactive server."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import threading
import time
import types

import pytest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('notebook_adapter',ROOT/'sdk/python/tsugumi/notebook.py')
notebook=importlib.util.module_from_spec(spec);spec.loader.exec_module(notebook)

class Failure(Exception):
    def __init__(self,kind,message):super().__init__(message);self.kind=kind

class Run:
    run_id='notebook-test'
    card='notebook'
    invocation=1
    def __init__(self,root):
        self.output_directory=root/'out';self.temporary_directory=root/'tmp'
        self.output_directory.mkdir(parents=True);self.temporary_directory.mkdir()
        self.stop=False;self.deadline=60000
    def cancelled(self):return self.stop
    def remaining_ms(self):return self.deadline

@pytest.fixture
def host(monkeypatch):
    monkeypatch.setitem(sys.modules,'tsugumi',types.SimpleNamespace(Failure=Failure))


def test_notebook_overrides_once_and_isolates_repeated_calls(tmp_path,host):
    source=tmp_path/'src';source.mkdir()
    (source/'helper.py').write_text('''def twice(value): return value * 2
''')
    (source/'notebook.py').write_text('''import marimo
app=marimo.App()
@app.cell
def configuration():
    value=0
    return value,
@app.cell
def count(value):
    from pathlib import Path
    counter=Path("counter.txt")
    counter.write_text(str(int(counter.read_text())+1) if counter.exists() else "1")
    from helper import twice
    result=twice(value)
    return result,counter,Path,twice
@app.cell
def show(result):
    import marimo as mo
    mo.md(f"Result: {result}")
    return mo,
''')
    snapshots=[]
    for i in [1,2]:
        run_info=Run(tmp_path/str(i));run_info.invocation=i;run_info.card=f"notebook-{i}"
        result,folder=notebook.run(types.SimpleNamespace(markdown=lambda value: None), run_info,source,'notebook.py',{'value':i},['result'])
        assert result=={'result':i*2}
        assert json.loads((folder/'metadata.json').read_text())['card']==f'notebook-{i}'
        assert (folder/'source/counter.txt').read_text()=='1'
        snapshots.append((folder,(folder/'index.html').read_bytes()))
    # A second committed run does not rewrite the first artifact.
    assert all((folder/'index.html').read_bytes()==content for folder,content in snapshots)


def test_cell_failure_exposes_notebook_source_and_no_result(tmp_path,host,capfd):
    source=tmp_path/'src';source.mkdir()
    (source/'broken.py').write_text('''import marimo
app=marimo.App()
@app.cell
def failing_cell():
    result=1/0
    return result,
''')
    run_info=Run(tmp_path/'call')
    with pytest.raises(Failure,match='broken.py') as e:
        notebook.run(types.SimpleNamespace(markdown=lambda value: None), run_info,source,'broken.py',{},['result'])
    assert e.value.kind=='worker_failed'
    output=capfd.readouterr()
    assert 'failing_cell (broken.py:4)' in output.out
    assert 'ZeroDivisionError' in output.err
    assert not (run_info.output_directory/'notebook/result.json').exists()


@pytest.mark.parametrize('kind',['cancelled','timeout'])
def test_notebook_child_is_reaped_on_cancel_or_deadline(tmp_path,host,kind):
    source=tmp_path/'src';source.mkdir()
    (source/'slow.py').write_text('''import os,time
from pathlib import Path
Path("pid").write_text(str(os.getpid()))
time.sleep(60)
''')
    run_info=Run(tmp_path/'call')
    timer=threading.Timer(1,lambda:setattr(run_info,'stop' if kind=='cancelled' else 'deadline',True if kind=='cancelled' else 0))
    timer.start()
    try:
        with pytest.raises(Failure) as e:notebook.run(types.SimpleNamespace(markdown=lambda value: None), run_info,source,'slow.py',{},[])
        assert e.value.kind==kind
    finally:timer.cancel()
    pid=int((run_info.temporary_directory/'marimo/notebook/source/pid').read_text())
    with pytest.raises(ProcessLookupError):os.kill(pid,0)
    assert not (run_info.output_directory / 'notebook').exists()
