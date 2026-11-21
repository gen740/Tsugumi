"""Go Cards own their buffers and guard callbacks for the life of each call."""
import base64
import json
import shutil

import pytest
from test_language_cards import project  # pytest fixture

from conftest import ROOT
from projects import run_and_wait, start_run, wait_run
from test_language_cards import scaffold_card, write_project as write_language_project
from tsugumi_client import line_chart_series, path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState

NIX='{ pkgs, inputs, params }: let sdk=inputs.tsugumi.sdk {languages=["go"];}; in {run=inputs.tsugumi.goCard {inherit sdk;src=./src;};}'
NUMBER='namespace t; table Number{value:ulong;} table Outputs{out:Number(required);} root_type Outputs;'


def write_project(root,cards,connections):
    write_language_project(root,cards,connections)
    for name in cards:
        p=root/'definitions'/name/'_card.json'
        data=json.loads(p.read_text());data['sandbox']=True;p.write_text(json.dumps(data))


def go(source,**card):
    if 'func onRunStart(' not in source:
        source+='''
func onRunStart(inputs tsugumi.Inputs) error { return nil }
'''
    return {'card':{'type':'code','language':'go',**card},'nix':NIX,'src':{
        'go.mod':(ROOT/'sdk/go/template/go.mod').read_text(),
        'go.sum':'','card.go':source}}


def shown(c,run,card):
    return c.call('RunRecordRequest',runId=run,card=path(card)).record.display


def succeeded(c):
    run,status=run_and_wait(c,'start')
    assert status.state==RunState.succeeded,(status.failure and text(status.failure.diagnostic),[
        (name,text(c.call('LogRangeRequest',runId=run,card=path(name),stream=stream,limitBytes=10000).text))
        for name in ('go','make','check','count','fail','show','live','slow') for stream in (2,4)])
    return run


def test_go_scaffold_executes(project):
    root, connect = project
    write_project(root, {'hello': scaffold_card('go')}, [('start', 'out', 'hello', 'in')])
    client = connect()
    assert text(shown(client, succeeded(client), 'hello').value.text) == 'hello'


def test_failing_go_test_rejects_the_card_build(project):
    root, connect = project
    card = go('''package main
import "tsugumi"
func run(inputs tsugumi.Inputs) (tsugumi.Output, error) { return tsugumi.Output{}, nil }
''')
    card['src']['checks/card_test.go'] = '''package checks
import "testing"
func TestCard(t *testing.T) { t.Fatal("deliberate Go Card test failure") }
'''
    write_project(root, {'go': card}, [])
    c = connect()
    run_id, status = run_and_wait(c, 'start')
    log = text(c.call('LogRangeRequest', runId=run_id, card=path('go'), stream=LogStream.build).text)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.nix_failed, log
    assert 'deliberate Go Card test failure' in text(status.failure.diagnostic) + log


def test_python_go_cpp_typed_values(project):
    root,connect=project
    card=go('''package main
import "tsugumi"
func run(inputs tsugumi.Inputs, report *tsugumi.Report, run *tsugumi.Run)(tsugumi.Output,error){
 p:=inputs.Params()
 if inputs.Number().Value!=9007199254740993 || inputs.Optional().Value!=inputs.Number().Value || *p.Offset!=7 {panic("typed input changed")}
 out:=&tsugumi.OutputOut{Value:inputs.Number().Value+*p.Offset}
 if err:=report.Scalar(float64(out.Value),"value");err!=nil{return tsugumi.Output{},err}
 return tsugumi.Output{Out:out},nil
}''',inputs=[{'name':'number'},{'name':'optional','optional':True}],params=[{'name':'offset','type':'uint64','data':7}])
    card['outputs']=NUMBER
    write_project(root,{
        'make':{'card':{'type':'code','language':'python'},'outputs':NUMBER.replace('out:Number(required);','out:Number(required);other:Number(required);'),
                'nix':'{pkgs,inputs,params}:let sdk=inputs.tsugumi.sdk {languages=["python"];};in {run=inputs.tsugumi.pythonCard {inherit sdk;src=./src;};}',
                'src':{'main.py':'''import tsugumi
import outputs.out.t.Number as N
def main(inputs):
 result = {}
 for port in ("out","other"):
  b=tsugumi.builder()
  N.Start(b)
  N.AddValue(b,9007199254740993)
  result[port] = tsugumi.message(b, N.End(b))
 return result
'''}},
        'go':card,
        'check':{'card':{'type':'code','language':'cpp','inputs':[{'name':'number'}]},
                 'nix':'{pkgs,inputs,params}:let sdk=inputs.tsugumi.sdk {languages=["cpp"];};in {run=inputs.tsugumi.cppCard {inherit sdk;src=./src;};}',
                 'src':{'card.cc':'''#include <card.hh>
auto run(const Card::Inputs& inputs, tsugumi::Report& report)->Card::Output {if(inputs.number.value!=9007199254741000ULL)throw std::runtime_error("wrong value");report.text("exact");return {};}'''}},
    },[('make','out','go','number'),('make','other','go','optional'),('go','out','check','number')])
    c=connect();run=succeeded(c)
    assert text(shown(c,run,'check').value.text)=='exact'
    assert shown(c, run, 'go').value.value == 9007199254741000


@pytest.mark.parametrize('reuse',[True,False])
def test_state_run_hook_and_stale_context(project,reuse):
    root,connect=project
    source='''package main
import("tsugumi";"fmt";"errors")
var runs,calls int
var old *tsugumi.Report
func onRunStart(inputs tsugumi.Inputs) error {runs++;return nil}
func run(inputs tsugumi.Inputs, report *tsugumi.Report, run *tsugumi.Run)(tsugumi.Output,error){
 if old!=nil && !errors.Is(old.Log("late"),tsugumi.ErrClosed){panic("stale callback reached host")}
 copy:=*report;old=&copy;calls++
 err:=run.Go(func()error{return report.Text(fmt.Sprintf("%d %d",runs,calls))})
 return tsugumi.Output{},err
}'''
    card = go(source, reuseWorker=reuse)
    card['nix'] = NIX.replace(
        'in {run=',
        "in { preRun = ''echo before-go >&2''; postRun = ''echo after-go >&2''; run=",
    )
    write_project(root, {'count': card}, [])
    c=connect()
    for n in (1,2):
        run=succeeded(c);expected=n if reuse else 1
        assert text(shown(c,run,'count').value.text)==f'{expected} {expected}'
        rec=c.call('RunInvocationsRequest',runId=run,card=path('count')).records[0]
        assert bool(rec.workerReused)==(reuse and n==2)
        log = text(c.call('LogRangeRequest', runId=run, card=path('count'), stream=LogStream.standard_error).text)
        assert 'before-go' in log and 'after-go' in log


@pytest.mark.parametrize('body,reason',[
    ('return tsugumi.Output{},tsugumi.Fail("output_invalid","on purpose")','on purpose'),
    ('panic("panic on purpose")','panic on purpose'),
    ('return tsugumi.Output{},run.Go(func()error{panic("task panic")})','task panic'),
    ('return tsugumi.Output{},badError{}','Go error reporting panicked'),
    ('go func(){panic("raw goroutine panic")}();select{}','raw goroutine panic'),
])
def test_error_and_panic_recovery(project,body,reason):
    root,connect=project
    source='''package main
import "tsugumi"
type badError struct{}
func(badError)Error()string{panic("error method panic")}
func run(inputs tsugumi.Inputs, report *tsugumi.Report, run *tsugumi.Run)(tsugumi.Output,error){
 p:=inputs.Params()
 if !*p.Fail{return tsugumi.Output{},report.Text("recovered")}
 BODY
}'''.replace('BODY',body)
    write_project(root,{'fail':go(source,params=[{'name':'fail','type':'bool','data':True}])},[])
    c=connect();run,status=run_and_wait(c,'start')
    assert status.state==RunState.failed
    kind = FailureKind.output_invalid if 'tsugumi.Fail(' in body else FailureKind.worker_failed
    assert status.failure.kind == kind
    assert reason in text(c.call('LogRangeRequest',runId=run,card=path('fail'),stream=2,limitBytes=65536).text)
    from test_demos import edit_card,set_param
    edit_card(None,c,'fail',lambda card:set_param(card,'fail',False))
    assert text(shown(c,succeeded(c),'fail').value.text)=='recovered'


def test_live_display_cancel_and_next_run(project):
    from test_demos import first_display,edit_card,set_param
    root,connect=project
    source='''package main
import "tsugumi"
func run(inputs tsugumi.Inputs, report *tsugumi.Report, run *tsugumi.Run)(tsugumi.Output,error){
 p:=inputs.Params()
 if !*p.Slow{return tsugumi.Output{},report.Text("recovered")}
 return tsugumi.Output{},run.Go(func()error{if err:=report.Text("running");err!=nil{return err};<-run.Done();return nil})
}'''
    write_project(root,{'live':go(source,params=[{'name':'slow','type':'bool','data':True}])},[])
    c=connect();opened=c.open()
    c.call('ViewSetVisibleRequest',subscriptionId=text(opened.subscriptionId),cards=[path('live')])
    run=start_run(c,'start');event=first_display(c,run)
    assert text(event.value.display.value.text)=='running'
    c.call('RunCancelRequest',runId=run)
    assert wait_run(c,run).state==RunState.cancelled
    edit_card(None,c,'live',lambda card:set_param(card,'slow',False))
    recovered=succeeded(c)
    assert text(shown(c,recovered,'live').value.text)=='recovered'
    assert not c.call('RunInvocationsRequest',runId=recovered,card=path('live')).records[0].workerReused


def test_deadline_stops_uncooperative_goroutine(project):
    from test_demos import edit_card,set_param
    root,connect=project
    source='''package main
import "tsugumi"
func run(inputs tsugumi.Inputs, report *tsugumi.Report, run *tsugumi.Run)(tsugumi.Output,error){
 p:=inputs.Params()
 if !*p.Slow{return tsugumi.Output{},report.Text("recovered")}
 if run.Remaining()<0{panic("no deadline")}
 return tsugumi.Output{},run.Go(func()error{for{};return nil})
}'''
    write_project(root,{'slow':go(source,timeoutSeconds=2,params=[{'name':'slow','type':'bool','data':True}])},[])
    c=connect();run,status=run_and_wait(c,'start')
    assert status.state==RunState.failed, (text(c.call('LogRangeRequest',runId=run,card=path('slow'),stream=2,limitBytes=65536).text), vars(c.call('RunRecordRequest',runId=run,card=path('slow')).status))
    assert c.call('RunRecordRequest',runId=run,card=path('slow')).status.failure.kind == FailureKind.timeout
    edit_card(None,c,'slow',lambda card:set_param(card,'slow',False))
    recovered=succeeded(c)
    assert text(shown(c,recovered,'slow').value.text)=='recovered'
    assert not c.call('RunInvocationsRequest',runId=recovered,card=path('slow')).records[0].workerReused


def test_file_directory_inputs_and_artifact_output(project):
    root,connect=project
    schema='attribute "tsugumi";namespace t;table Path{value:string;}table Outputs{file:Path(required,tsugumi:"file");directory:Path(required,tsugumi:"directory");}root_type Outputs;'
    make='''{pkgs,inputs,params}:{run=''
      mkdir -p "$TSUGUMI_OUTPUT_DIR/d"
      echo hello > "$TSUGUMI_OUTPUT_DIR/f"
      echo world > "$TSUGUMI_OUTPUT_DIR/d/value"
      jq -n --arg f "$TSUGUMI_OUTPUT_DIR/f" --arg d "$TSUGUMI_OUTPUT_DIR/d" '{file:{value:$f},directory:{value:$d}}' > "$TSUGUMI_OUTPUT"
    ''; }'''
    card=go('''package main
import("tsugumi";"os";"path/filepath")
func run(inputs tsugumi.Inputs, report *tsugumi.Report, run *tsugumi.Run)(tsugumi.Output,error){
 a,err:=os.ReadFile(inputs.File().Value);if err!=nil{return tsugumi.Output{},err}
 b,err:=os.ReadFile(filepath.Join(inputs.Directory().Value,"value"));if err!=nil{return tsugumi.Output{},err}
 file:=filepath.Join(run.OutputDirectory(),"report.txt")
 if err:=os.WriteFile(file,append(a,b...),0600);err!=nil{return tsugumi.Output{},err}
 return tsugumi.Output{Report:&tsugumi.OutputReport{Value:file}},nil
}''',inputs=[{'name':'file'},{'name':'directory'}],retainArtifact=True,artifactOutput='report')
    card['outputs']='attribute "tsugumi";namespace t;table Path{value:string;}table Outputs{report:Path(required,tsugumi:"file");}root_type Outputs;'
    write_project(root,{'make':{'card':{'type':'code','language':'bash'},'nix':make,'outputs':schema},'go':card},[('make',p,'go',p) for p in ('file','directory')])
    c=connect()
    run=succeeded(c)
    artifact=next(a for a in c.call('ArtifactListRequest',runId=run).artifacts if text(a.output)=='report')
    assert bytes(c.call('ArtifactReadRequest',artifactId=text(artifact.artifactId)).bytes)==b'hello\nworld\n'


PNG=','.join(str(b) for b in base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a2ioAAAAASUVORK5CYII='))


def test_go_displays_preserve_payloads(project):
    from test_demos import edit_card, set_param

    root, connect = project
    cases = [
        ('report.Markdown("**hello**")', (lambda view: (view.markdown, text(view.text)), (True, "**hello**"))),
        ('report.LineChart("line","value",[]float64{1,2},[]float64{3,4})', (lambda view: tuple(line_chart_series(view)[0].y), (3.0, 4.0))),
        ('report.Table([]string{"value"},[][]string{{"1"}})', (lambda view: text(view.rows[0].cells[0]), "1")),
        ('report.Image("image/png",[]byte{'+PNG+'},"image")', (lambda view: bytes(view.data)[:8], b"\x89PNG\r\n\x1a\n")),
        ('report.Points([]float32{1},[]float32{2},[]float32{3},[]uint32{0xff00ff})', (lambda view: (list(view.z), list(view.colors)), ([3.0], [0xff00ff]))),
        ('report.Module("vega-lite",`{"mark":"point","data":{"values":[]}}`)', (lambda view: json.loads(text(view.configJson))["mark"], "point")),
    ]
    arms = '\n'.join(f'case {index}: return tsugumi.Output{{}}, {expression}'
                     for index, (expression, _) in enumerate(cases))
    source = '''package main
import "tsugumi"
func run(inputs tsugumi.Inputs, report *tsugumi.Report)(tsugumi.Output,error){
 switch *inputs.Params().Mode {
 ARMS
 }
 panic("unknown display mode")
}
'''.replace('ARMS', arms)
    card = go(source, params=[{'name': 'mode', 'type': 'uint64', 'data': 0}])
    write_project(root, {'show': card}, [])
    client = connect()
    for index, (expression, (decode, expected)) in enumerate(cases):
        if index:
            edit_card(None, client, 'show', lambda card: set_param(card, 'mode', index))
        display = shown(client, succeeded(client), 'show')
        assert decode(display.value) == expected, expression


def test_managed_output_task_completes_before_outputs_are_packed(project):
    root,connect=project
    card=go('''package main
import "tsugumi"
func run(inputs tsugumi.Inputs, report *tsugumi.Report, run *tsugumi.Run)(tsugumi.Output,error){
 out:=&tsugumi.OutputOut{}
 returning:=make(chan struct{})
 defer close(returning)
 err:=run.Go(func()error{
   <-returning
   out.Value=inputs.Number().Value
   return nil
 })
 return tsugumi.Output{Out:out},err
}''',inputs=[{'name':'number'}])
    card['outputs']=NUMBER
    # Stronger runtime cgo checks ensure callbacks do not retain Go pointers.
    card['nix']=NIX.replace('run=inputs.tsugumi.goCard {inherit sdk;src=./src;};', 'run=(inputs.tsugumi.goCard {inherit sdk;src=./src;}).overrideAttrs (old:{env=old.env // {GOEXPERIMENT="cgocheck2";};});')
    make='{pkgs,inputs,params}:{run=\'\'echo \'{"out":{"value":9007199254740993}}\' > "$TSUGUMI_OUTPUT"\'\';}'
    check='{pkgs,inputs,params}:{run=\'\'jq -n --rawfile v "$JSON_NUMBER" \'{text:$v}\' > "$TSUGUMI_REPORT"\'\';}'
    write_project(root,{'make':{'card':{'type':'code','language':'bash'},'outputs':NUMBER,'nix':make},'go':card,
                        'check':{'card':{'type':'code','language':'bash','inputs':[{'name':'number'}]},'nix':check}},[('make','out','go','number'),('go','out','check','number')])
    c=connect()
    assert json.loads(text(shown(c,succeeded(c),'check').value.text))=={'value':9007199254740993}


def external_module_card(arguments):
    card=go('''package main
import("tsugumi";"github.com/google/uuid")
func run(inputs tsugumi.Inputs, report *tsugumi.Report, run *tsugumi.Run)(tsugumi.Output,error){
 return tsugumi.Output{},report.Text(uuid.MustParse("550e8400-e29b-41d4-a716-446655440000").String())
}''')
    card['src']['go.mod']='''module tsugumi-card

go 1.26

require tsugumi v0.0.0

replace tsugumi => ./tsugumi

require github.com/google/uuid v1.6.0
'''
    card['src']['go.sum']='''github.com/google/uuid v1.6.0 h1:NIvaJDMOsjHA8n1jAhLSgzrAzy1Hgr+hNrb57e+94F0=
github.com/google/uuid v1.6.0/go.mod h1:TIyPZe4MgqvfeYDBFedMoGGpEw/LqOeaOT+nhxU+yHo=
'''
    card['nix'] = NIX.replace('src=./src;', 'src=./src;' + arguments)
    return card


UUID_HASH = "sha256-SuU3c1yjGaE9bhQm0Tr6F16h2l2f35a6dpSC7vGaZ5o="


def test_external_module_is_pinned_and_built_offline(project):
    root, connect = project
    card = external_module_card(f'lock = ./src/go.sum; hash = "{UUID_HASH}";')
    write_project(root, {'go': card}, [])
    c = connect()
    assert text(shown(c, succeeded(c), 'go').value.text) == '550e8400-e29b-41d4-a716-446655440000'


@pytest.mark.parametrize('arguments', [
    'lock = ./src/go.sum;',
    'lock = ./src/go.sum; hash = pkgs.lib.fakeHash;',
], ids=['missing-hash', 'wrong-hash'])
def test_go_rejects_an_invalid_pin(project, arguments):
    root, connect = project
    write_project(root, {'go': external_module_card(arguments)}, [])
    c = connect()
    run_id, status = run_and_wait(c, 'start')
    log = text(c.call('LogRangeRequest', runId=run_id, card=path('go'), stream=LogStream.build).text)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.nix_failed, log
    assert not c.call('RunRecordRequest', runId=run_id, card=path('go')).record.summary.environmentId


def test_go_rejects_missing_required_output(project):
    root, connect = project
    card = go('''package main
import "tsugumi"
func run(inputs tsugumi.Inputs)(tsugumi.Output,error){
 return tsugumi.Output{},nil
}''')
    card['outputs'] = NUMBER
    write_project(root, {'go': card}, [])
    _, status = run_and_wait(connect(), 'start')
    assert status.state == RunState.failed and status.failure.kind == FailureKind.output_invalid


def test_bundled_go_pipeline_runs_in_the_sandbox(project):
    root, connect = project
    shutil.copytree(ROOT / 'demos' / 'en' / 'go-pipeline', root, dirs_exist_ok=True)
    c = connect()
    run_id = succeeded(c)
    assert text(shown(c, run_id, 'check').value.text) == '21'
