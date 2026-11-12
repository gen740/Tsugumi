"""Go Cards own their buffers and guard callbacks for the life of each call."""
import base64
import json
import subprocess

import pytest

from conftest import ROOT
from projects import run_and_wait, start_run, wait_run
from test_language_cards import project, write_project as write_language_project
from tsugumi_client import path, text
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
        source+='\nfunc onRunStart(ctx *tsugumi.Context) error { return nil }\n'
    return {'card':{'language':'go',**card},'nix':NIX,'src':{
        'go.mod':(ROOT/'sdk/go/template/go.mod').read_text(),
        'go.sum':'','card.go':source}}


def shown(c,run,card):
    return c.call('RunRecordRequest',runId=run,card=path(card)).record.display


def succeeded(c):
    run,status=run_and_wait(c,'start',timeout=1800)
    assert status.state==RunState.succeeded,(status.failure and text(status.failure.diagnostic),[
        (name,text(c.call('LogRangeRequest',runId=run,card=path(name),stream=stream,limitBytes=10000).text))
        for name in ('go','make','check','count','fail','show','live','slow') for stream in (2,4)])
    return run


def test_scaffold_is_current_and_executes(project):
    subprocess.run(['python3',str(ROOT/'sdk/codegen/scaffold_go.py'),'--check'],check=True)
    root,connect=project
    card=go((ROOT/'sdk/go/template/card.go').read_text())
    card['outputs']='namespace hello;table Text{value:string;}table Outputs{out:Text(required);}root_type Outputs;'
    write_project(root,{'go':card},[])
    c=connect()
    assert text(shown(c,succeeded(c),'go').value.text)=='hello'


def test_python_go_cpp_typed_values(project):
    root,connect=project
    card=go('''package main
import "tsugumi"
func run(ctx *tsugumi.Context,input tsugumi.Input)(tsugumi.Output,error){
 p,err:=ctx.Params();if err!=nil{return tsugumi.Output{},err}
 if input.Number.Value!=9007199254740993 || input.Optional.Value!=input.Number.Value || *p.Offset!=7 {panic("typed input changed")}
 out:=&tsugumi.OutputOut{Value:input.Number.Value+*p.Offset}
 if err:=ctx.Log("Go received exact uint64");err!=nil{return tsugumi.Output{},err}
 if err:=ctx.DisplayScalar(float64(out.Value),"value");err!=nil{return tsugumi.Output{},err}
 return tsugumi.Output{Out:out},nil
}''',inputs=[{'name':'number'},{'name':'optional','optional':True}],params=[{'name':'offset','type':'uint64','data':7}])
    card['outputs']=NUMBER
    write_project(root,{
        'make':{'card':{'language':'python'},'outputs':NUMBER.replace('out:Number(required);','out:Number(required);other:Number(required);'),
                'nix':'{pkgs,inputs,params}:let sdk=inputs.tsugumi.sdk {languages=["python"];};in {run=inputs.tsugumi.pythonCard {inherit sdk;src=./src;};}',
                'src':{'main.py':'import outputs.out.t.Number as N\ndef main(ctx):\n for port in ("out","other"):\n  b=ctx.builder()\n  N.Start(b)\n  N.AddValue(b,9007199254740993)\n  ctx.output(port,b,N.End(b))\n'}},
        'go':card,
        'check':{'card':{'language':'cpp','inputs':[{'name':'number'}]},
                 'nix':'{pkgs,inputs,params}:let sdk=inputs.tsugumi.sdk {languages=["cpp"];};in {run=inputs.tsugumi.cppCard {inherit sdk;src=./src;};}',
                 'src':{'card.cc':'#include <card.hh>\nauto run(Card::Input& input)->Card::Output {if(input.number.value!=9007199254741000ULL)throw std::runtime_error("wrong value");tsugumi::visualization::text("exact");return {};}'}},
    },[('make','out','go','number'),('make','other','go','optional'),('go','out','check','number')])
    c=connect();run=succeeded(c)
    assert text(shown(c,run,'check').value.text)=='exact'
    assert 'exact uint64' in text(c.call('LogRangeRequest',runId=run,card=path('go'),stream=2,limitBytes=65536).text)


@pytest.mark.parametrize('reuse',[True,False])
def test_state_run_hook_and_stale_context(project,reuse):
    root,connect=project
    source='''package main
import("tsugumi";"fmt";"errors")
var runs,calls int
var old *tsugumi.Context
func onRunStart(ctx *tsugumi.Context) error {runs++;return nil}
func run(ctx *tsugumi.Context,_ tsugumi.Input)(tsugumi.Output,error){
 if old!=nil && !errors.Is(old.Log("late"),tsugumi.ErrClosed){panic("stale callback reached host")}
 copy:=*ctx;old=&copy;calls++
 err:=ctx.Go(func()error{return ctx.DisplayText(fmt.Sprintf("%d %d",runs,calls))})
 return tsugumi.Output{},err
}'''
    write_project(root,{'count':go(source,reuseWorker=reuse)},[])
    c=connect()
    for n in (1,2):
        run=succeeded(c);expected=n if reuse else 1
        assert text(shown(c,run,'count').value.text)==f'{expected} {expected}'
        rec=c.call('RunInvocationsRequest',runId=run,card=path('count')).records[0]
        assert bool(rec.workerReused)==(reuse and n==2)


@pytest.mark.parametrize('body,reason',[
    ('return tsugumi.Output{},tsugumi.Fail("output_invalid","on purpose")','on purpose'),
    ('panic("panic on purpose")','panic on purpose'),
    ('return tsugumi.Output{},ctx.Go(func()error{panic("task panic")})','task panic'),
    ('return tsugumi.Output{},badError{}','Go error reporting panicked'),
])
def test_error_and_panic_recovery(project,body,reason):
    root,connect=project
    source='''package main
import "tsugumi"
type badError struct{}
func(badError)Error()string{panic("error method panic")}
func run(ctx *tsugumi.Context,_ tsugumi.Input)(tsugumi.Output,error){
 p,err:=ctx.Params();if err!=nil{return tsugumi.Output{},err}
 if !*p.Fail{return tsugumi.Output{},ctx.DisplayText("recovered")}
 BODY
}'''.replace('BODY',body)
    write_project(root,{'fail':go(source,params=[{'name':'fail','type':'bool','data':True}])},[])
    c=connect();run,status=run_and_wait(c,'start',timeout=1800)
    assert status.state==RunState.failed
    assert reason in text(c.call('LogRangeRequest',runId=run,card=path('fail'),stream=2,limitBytes=65536).text)
    from test_demos import edit_card,set_param
    edit_card(None,c,'fail',lambda card:set_param(card,'fail',False))
    assert text(shown(c,succeeded(c),'fail').value.text)=='recovered'


def test_live_display_cancel_and_next_run(project):
    from test_demos import first_display,edit_card,set_param
    root,connect=project
    source='''package main
import "tsugumi"
func run(ctx *tsugumi.Context,_ tsugumi.Input)(tsugumi.Output,error){
 p,err:=ctx.Params();if err!=nil{return tsugumi.Output{},err}
 if !*p.Slow{return tsugumi.Output{},ctx.DisplayText("recovered")}
 return tsugumi.Output{},ctx.Go(func()error{if err:=ctx.DisplayText("running");err!=nil{return err};<-ctx.Done();return nil})
}'''
    write_project(root,{'live':go(source,params=[{'name':'slow','type':'bool','data':True}])},[])
    c=connect();opened=c.open()
    c.call('ViewSetVisibleRequest',subscriptionId=text(opened.subscriptionId),cards=[path('live')])
    run=start_run(c,'start');event=first_display(c,run)
    assert text(event.value.display.value.text)=='running'
    c.call('RunCancelRequest',runId=run)
    assert wait_run(c,run,10).state==RunState.cancelled
    edit_card(None,c,'live',lambda card:set_param(card,'slow',False))
    recovered=succeeded(c)
    assert text(shown(c,recovered,'live').value.text)=='recovered'
    assert not c.call('RunInvocationsRequest',runId=recovered,card=path('live')).records[0].workerReused


def test_deadline_stops_uncooperative_goroutine(project):
    from test_demos import edit_card,set_param
    root,connect=project
    source='''package main
import "tsugumi"
func run(ctx *tsugumi.Context,_ tsugumi.Input)(tsugumi.Output,error){
 p,err:=ctx.Params();if err!=nil{return tsugumi.Output{},err}
 if !*p.Slow{return tsugumi.Output{},ctx.DisplayText("recovered")}
 if ctx.Remaining()<0{panic("no deadline")}
 return tsugumi.Output{},ctx.Go(func()error{for{};return nil})
}'''
    write_project(root,{'slow':go(source,timeoutSeconds=2,params=[{'name':'slow','type':'bool','data':True}])},[])
    c=connect();run,status=run_and_wait(c,'start',timeout=1800)
    assert status.state==RunState.failed, (text(c.call('LogRangeRequest',runId=run,card=path('slow'),stream=2,limitBytes=65536).text), vars(c.call('RunRecordRequest',runId=run,card=path('slow')).status))
    assert c.call('RunRecordRequest',runId=run,card=path('slow')).status.failure.kind==14
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
func run(ctx *tsugumi.Context,input tsugumi.Input)(tsugumi.Output,error){
 a,err:=os.ReadFile(input.File.Value);if err!=nil{return tsugumi.Output{},err}
 b,err:=os.ReadFile(filepath.Join(input.Directory.Value,"value"));if err!=nil{return tsugumi.Output{},err}
 file:=filepath.Join(ctx.OutputDirectory(),"report.txt")
 if err:=os.WriteFile(file,append(a,b...),0600);err!=nil{return tsugumi.Output{},err}
 return tsugumi.Output{Report:&tsugumi.OutputReport{Value:file}},nil
}''',inputs=[{'name':'file'},{'name':'directory'}],retainArtifact=True,artifactOutput='report')
    card['outputs']='attribute "tsugumi";namespace t;table Path{value:string;}table Outputs{report:Path(required,tsugumi:"file");}root_type Outputs;'
    write_project(root,{'make':{'card':{'language':'bash'},'nix':make,'outputs':schema},'go':card},[('make',p,'go',p) for p in ('file','directory')])
    c=connect()
    for _ in range(2):
        run=succeeded(c)
        artifact=next(a for a in c.call('ArtifactListRequest',runId=run).artifacts if text(a.output)=='report')
        assert bytes(c.call('ArtifactReadRequest',artifactId=text(artifact.artifactId)).bytes)==b'hello\nworld\n'


PNG=','.join(str(b) for b in base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a2ioAAAAASUVORK5CYII='))
@pytest.mark.parametrize('expression,kind',[
    ('ctx.DisplayText("hello")',1),
    ('ctx.DisplayMarkdown("**hello**")',1),
    ('ctx.DisplayScalar(1.5,"value")',2),
    ('ctx.DisplayLineChart("line","value",[]float64{1,2},[]float64{3,4})',3),
    ('ctx.DisplayTable([]string{"value"},[][]string{{"1"}})',4),
    ('ctx.DisplayImage("image/png",[]byte{'+PNG+'},"image")',5),
    ('ctx.DisplayPoints([]float32{1},[]float32{2},[]float32{3},[]uint32{0xff00ff})',6),
    ('ctx.DisplayModule("vega-lite",`{"mark":"point","data":{"values":[]}}`)',7),
])
def test_displays(project,expression,kind):
    root,connect=project
    source='package main\nimport "tsugumi"\nfunc run(ctx *tsugumi.Context,_ tsugumi.Input)(tsugumi.Output,error){return tsugumi.Output{},'+expression+'}'
    write_project(root,{'show':go(source)},[])
    c=connect();assert shown(c,succeeded(c),'show').valueType==kind


def test_owned_inputs_and_managed_output_task(project):
    root,connect=project
    card=go('''package main
import("tsugumi";"time")
var previous tsugumi.Input
func run(ctx *tsugumi.Context,input tsugumi.Input)(tsugumi.Output,error){
 if previous.Number!=nil && previous.Number.Value!=9007199254740993 {panic("retained input changed")}
 previous=input
 out:=&tsugumi.OutputOut{}
 err:=ctx.Go(func()error{time.Sleep(time.Millisecond);out.Value=input.Number.Value;return nil})
 return tsugumi.Output{Out:out},err
}''',inputs=[{'name':'number'}])
    card['outputs']=NUMBER
    # Stronger runtime cgo checks ensure callbacks do not retain Go pointers.
    card['nix']=NIX.replace('run=inputs.tsugumi.goCard {inherit sdk;src=./src;};', 'run=(inputs.tsugumi.goCard {inherit sdk;src=./src;}).overrideAttrs (old:{env=old.env // {GOEXPERIMENT="cgocheck2";};});')
    make='{pkgs,inputs,params}:{run=\'\'echo \'{"out":{"value":9007199254740993}}\' > "$TSUGUMI_OUTPUT"\'\';}'
    check='{pkgs,inputs,params}:{run=\'\'jq -n --rawfile v "$JSON_NUMBER" \'{text:$v}\' > "$TSUGUMI_DISPLAY"\'\';}'
    write_project(root,{'make':{'card':{'language':'bash'},'outputs':NUMBER,'nix':make},'go':card,
                        'check':{'card':{'language':'bash','inputs':[{'name':'number'}]},'nix':check}},[('make','out','go','number'),('go','out','check','number')])
    c=connect()
    for _ in range(2):
        assert json.loads(text(shown(c,succeeded(c),'check').value.text))=={'value':9007199254740993}


def test_external_module_is_pinned_and_built_offline(project):
    root,connect=project
    card=go('package main\nimport("tsugumi";"github.com/google/uuid")\nfunc run(ctx *tsugumi.Context,_ tsugumi.Input)(tsugumi.Output,error){\n return tsugumi.Output{},ctx.DisplayText(uuid.MustParse("550e8400-e29b-41d4-a716-446655440000").String())\n}')
    card['src']['go.mod']='module tsugumi-card\n\ngo 1.26\n\nrequire tsugumi v0.0.0\n\nreplace tsugumi => ./tsugumi\n\nrequire github.com/google/uuid v1.6.0\n'
    card['src']['go.sum']='github.com/google/uuid v1.6.0 h1:NIvaJDMOsjHA8n1jAhLSgzrAzy1Hgr+hNrb57e+94F0=\ngithub.com/google/uuid v1.6.0/go.mod h1:TIyPZe4MgqvfeYDBFedMoGGpEw/LqOeaOT+nhxU+yHo=\n'
    card['nix']=NIX.replace('src=./src;','src=./src;vendorHash="sha256-SuU3c1yjGaE9bhQm0Tr6F16h2l2f35a6dpSC7vGaZ5o=";')
    write_project(root,{'go':card},[])
    c=connect()
    assert text(shown(c,succeeded(c),'go').value.text)=='550e8400-e29b-41d4-a716-446655440000'
