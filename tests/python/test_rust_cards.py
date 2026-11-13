"""Typed Rust Cards use the same worker, sandbox and output validation as C++."""
import json
import shutil
import subprocess
from pathlib import Path

import pytest

from conftest import ROOT
from projects import run_and_wait, start_run, wait_run
from test_language_cards import project, write_project as write_language_project
from tsugumi_client import path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState

NIX = '{ pkgs, inputs, params }: let sdk = inputs.tsugumi.sdk { languages = [ "rust" ]; }; in { run = inputs.tsugumi.rustCard { inherit sdk; src = ./src; }; }'
NUMBER = 'namespace t; table Number { value:ulong; } table Outputs { out:Number (required); } root_type Outputs;'


def write_project(root, cards, connections):
    write_language_project(root, cards, connections)
    # Exercise the actual host sandbox (Seatbelt here, bubblewrap on Linux/VM).
    for name in cards:
        file = root / 'definitions' / name / '_card.json'
        definition = json.loads(file.read_text())
        definition['sandbox'] = True
        file.write_text(json.dumps(definition))


def rust(source, **card):
    template = ROOT / 'sdk/rust/template'
    return {'card': {'language': 'rust', **card}, 'nix': NIX, 'src': {
        'Cargo.toml': (template / 'Cargo.toml').read_text(),
        'src/lib.rs': source,
    }}


def shown(c, run_id, card):
    return c.call('RunRecordRequest', runId=run_id, card=path(card)).record.display


def succeeded(c):
    run_id, status = run_and_wait(c, 'start', timeout=1800)
    assert status.state == RunState.succeeded, (status.failure and text(status.failure.diagnostic), [(name, text(c.call("LogRangeRequest", runId=run_id, card=path(name), stream=2, limitBytes=12000).text)) for name in ("make", "rust", "check", "fail", "show")])
    return run_id


def test_scaffold_generated_from_one_template():
    subprocess.run(['python3', str(ROOT / 'sdk/codegen/scaffold_rust.py'), '--check'], check=True)


def test_python_rust_cpp_values_and_params(project):
    root, connect = project
    process = rust('''use tsugumi::{Context, Input, Output, Result};
fn run(ctx: &Context<'_>, input: Input<'_>) -> Result<Output> {
    assert_eq!(input.optional.unwrap().value(), 9007199254740993);
    assert_eq!(input.number.value(), 9007199254740993);
    assert_eq!(ctx.params()?.offset(), Some(7));
    assert!(!ctx.cancelled());
    ctx.log("Rust received exact uint64")?;
    let mut out = tsugumi::outputs::Out::default();
    out.value = input.number.value() + ctx.params()?.offset().unwrap();
    ctx.display_table(&["value"], &[vec![out.value.to_string()]])?;
    Ok(Output {out: Some(out)})
}
tsugumi::export!(run);''', inputs=[{'name': 'number'}, {'name': 'optional', 'optional': True}], params=[{'name': 'offset', 'type': 'uint64', 'data': 7}])
    process['outputs'] = NUMBER
    write_project(root, {
        'make': {'card': {'language': 'python'}, 'outputs': NUMBER.replace('out:Number (required);', 'out:Number (required); other:Number (required);'),
                 'nix': '{ pkgs, inputs, params }: let sdk = inputs.tsugumi.sdk { languages = [ "python" ]; }; in {run = inputs.tsugumi.pythonCard {inherit sdk; src=./src;};}',
                 'src': {'main.py': 'import outputs.out.t.Number as N\ndef main(ctx):\n b=ctx.builder()\n N.Start(b)\n N.AddValue(b,9007199254740993)\n root=N.End(b)\n ctx.output("out",b,root)\n b=ctx.builder()\n N.Start(b)\n N.AddValue(b,9007199254740993)\n ctx.output("other",b,N.End(b))\n'}},
        'rust': process,
        'check': {'card': {'language': 'cpp', 'inputs': [{'name': 'number'}]},
                  'nix': '{ pkgs, inputs, params }: let sdk=inputs.tsugumi.sdk {languages=["cpp"];}; in {run=inputs.tsugumi.cppCard {inherit sdk;src=./src;};}',
                  'src': {'card.cc': '#include <card.hh>\n#include <stdexcept>\nauto run(Card::Input& input) -> Card::Output { if(input.number.value != 9007199254741000ULL) throw std::runtime_error("wrong value"); return {}; }'}},
    }, [('make', 'out', 'rust', 'number'), ('make', 'other', 'rust', 'optional'), ('rust', 'out', 'check', 'number')])
    c = connect()
    run_id = succeeded(c)
    assert text(shown(c, run_id, 'rust').value.rows[0].cells[0]) == '9007199254741000'
    assert 'exact uint64' in text(c.call('LogRangeRequest', runId=run_id, card=path('rust'), stream=2, limitBytes=65536).text)


@pytest.mark.parametrize('reuse', [True, False])
def test_state_and_run_hook(project, reuse):
    root, connect = project
    source = '''use tsugumi::{Context, Input, Output, Result};
use std::sync::atomic::{AtomicU64, Ordering};
static RUNS: AtomicU64 = AtomicU64::new(0);
static CALLS: AtomicU64 = AtomicU64::new(0);
fn start(_: &Context<'_>) -> Result<()> { RUNS.fetch_add(1, Ordering::Relaxed); Ok(()) }
fn run(ctx: &Context<'_>, _: Input<'_>) -> Result<Output> {
 let calls=CALLS.fetch_add(1, Ordering::Relaxed)+1;
 ctx.display_text(&format!("{} {}",RUNS.load(Ordering::Relaxed),calls))?;
 Ok(Output::default())
}
tsugumi::export!(run,start);'''
    write_project(root, {'count': rust(source, reuseWorker=reuse)}, [])
    c = connect()
    for n in (1, 2):
        run_id = succeeded(c)
        value = n if reuse else 1
        assert text(shown(c, run_id, 'count').value.text) == f'{value} {value}'
        record = c.call('RunInvocationsRequest', runId=run_id, card=path('count')).records[0]
        assert bool(record.workerReused) == (reuse and n == 2)


@pytest.mark.parametrize('body,reason', [
    ('Err(tsugumi::Error::new("output_invalid", "on purpose"))', 'on purpose'),
    ('panic!("panic on purpose")', 'panic on purpose'),
    ('struct Payload; impl Drop for Payload { fn drop(&mut self) { panic!("drop"); } } std::panic::panic_any(Payload)', 'Rust Card panicked'),
])
def test_failures_stay_inside_ffi_and_next_run_recovers(project, body, reason):
    root, connect = project
    source = '''use tsugumi::{Context, Input, Output, Result};
fn run(ctx:&Context<'_>, _:Input<'_>)->Result<Output> {
 if !ctx.params()?.fail().unwrap_or(false) { ctx.display_text("recovered")?; return Ok(Output::default()); }
 BODY
}
tsugumi::export!(run);'''.replace('BODY', body)
    write_project(root, {'fail': rust(source, params=[{'name': 'fail', 'type': 'bool', 'data': True}])}, [])
    c = connect()
    run_id, status = run_and_wait(c, 'start', timeout=1800)
    assert status.state == RunState.failed
    assert reason in text(c.call('LogRangeRequest', runId=run_id, card=path('fail'), stream=2, limitBytes=65536).text)
    from test_demos import edit_card, set_param
    edit_card(None, c, 'fail', lambda card: set_param(card, 'fail', False))
    run_id = succeeded(c)
    assert text(shown(c, run_id, 'fail').value.text) == 'recovered'


def test_live_display_and_cancellation(project):
    from test_demos import first_display
    root, connect = project
    source = '''use tsugumi::{Context,Input,Output,Result};
fn run(ctx:&Context<'_>,_:Input<'_>)->Result<Output> {
 if !ctx.params()?.slow().unwrap_or(false) { ctx.display_text("recovered")?; return Ok(Output::default()); }
 ctx.display_text("running")?;
 while !ctx.cancelled() {std::thread::sleep(std::time::Duration::from_millis(10));}
 Ok(Output::default())
}
tsugumi::export!(run);'''
    write_project(root, {'live': rust(source, params=[{'name':'slow','type':'bool','data':True}])}, [])
    c=connect()
    opened=c.open()
    c.call('ViewSetVisibleRequest', subscriptionId=text(opened.subscriptionId), cards=[path('live')])
    run_id=start_run(c,'start')
    notification=first_display(c,run_id)
    assert text(notification.value.display.value.text) == 'running'
    c.call('RunCancelRequest',runId=run_id)
    assert wait_run(c,run_id,10).state == RunState.cancelled
    from test_demos import edit_card, set_param
    edit_card(None,c,'live',lambda card:set_param(card,'slow',False))
    assert text(shown(c,succeeded(c),'live').value.text)=='recovered'


def test_deadline_kills_uncooperative_rust(project):
    root, connect = project
    write_project(root, {'slow': rust('''use tsugumi::{Context,Input,Output,Result};
fn run(ctx:&Context<'_>,_:Input<'_>)->Result<Output> {
 if !ctx.params()?.slow().unwrap_or(false) { ctx.display_text("recovered")?; return Ok(Output::default()); }
 assert!(ctx.remaining_ms().is_some());
 loop { std::hint::spin_loop(); }
}
tsugumi::export!(run);''', timeoutSeconds=2, params=[{'name':'slow','type':'bool','data':True}])}, [])
    c=connect()
    run_id,status=run_and_wait(c,'start',timeout=1800)
    assert status.state == RunState.failed
    assert c.call('RunRecordRequest',runId=run_id,card=path('slow')).status.failure.kind == FailureKind.timeout
    from test_demos import edit_card, set_param
    edit_card(None,c,'slow',lambda card:set_param(card,'slow',False))
    assert text(shown(c,succeeded(c),'slow').value.text)=='recovered'


def test_file_directory_inputs_and_artifact_output(project):
    root, connect = project
    schema = '''attribute "tsugumi"; namespace t; table Path { value:string; }
    table Outputs { file:Path (required, tsugumi:"file"); directory:Path (required, tsugumi:"directory"); }
    root_type Outputs;'''
    make = '''{ pkgs, inputs, params }: { run = ''
      mkdir -p "$TSUGUMI_OUTPUT_DIR/d"
      echo hello > "$TSUGUMI_OUTPUT_DIR/f"
      echo world > "$TSUGUMI_OUTPUT_DIR/d/value"
      jq -n --arg f "$TSUGUMI_OUTPUT_DIR/f" --arg d "$TSUGUMI_OUTPUT_DIR/d" '{file:{value:$f},directory:{value:$d}}' > "$TSUGUMI_OUTPUT"
    ''; }'''
    card=rust('''use tsugumi::{Context,Input,Output,Result};
fn run(ctx:&Context<'_>,input:Input<'_>)->Result<Output> {
 let first=std::fs::read_to_string(input.file.value().unwrap())?;
 let second=std::fs::read_to_string(std::path::Path::new(input.directory.value().unwrap()).join("value"))?;
 assert_eq!(first,"hello\\n"); assert_eq!(second,"world\\n");
 let file=ctx.output_directory().join("report.txt");
 std::fs::write(&file,first+&second)?;
 let mut report=tsugumi::outputs::Report::default();
 report.value=Some(file.to_str().unwrap().to_owned());
 Ok(Output{report:Some(report)})
}
tsugumi::export!(run);''', inputs=[{'name':'file'},{'name':'directory'}], retainArtifact=True, artifactOutput='report')
    card['outputs']='attribute "tsugumi"; namespace t; table Path{value:string;} table Outputs{report:Path(required,tsugumi:"file");} root_type Outputs;'
    write_project(root,{'make':{'card':{'language':'bash'},'nix':make,'outputs':schema},'rust':card},[('make',p,'rust',p) for p in ('file','directory')])
    c=connect()
    for _ in range(2):
        run_id=succeeded(c)
        art=next(a for a in c.call('ArtifactListRequest',runId=run_id).artifacts if text(a.output)=='report')
        assert bytes(c.call('ArtifactReadRequest',artifactId=text(art.artifactId)).bytes)==b'hello\nworld\n'


@pytest.mark.parametrize('expression,kind', [
    ('ctx.display_text("hello")',1),
    ('ctx.display_markdown("**hello**")',1),
    ('ctx.display_scalar(1.5,"value")',2),
    ('ctx.display_line_chart("one","line",&[1.0,2.0],&[3.0,4.0])',7),
    ('ctx.display_table(&["value"],&[vec!["1".into()]])',4),
    ('ctx.display_image("image/png", &[137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 1, 0, 0, 0, 1, 8, 4, 0, 0, 0, 181, 28, 12, 2, 0, 0, 0, 11, 73, 68, 65, 84, 120, 218, 99, 252, 255, 31, 0, 3, 3, 2, 0, 239, 154, 218, 42, 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130], "image")',5),
    ('ctx.display_points(&[1.0],&[2.0],&[3.0],Some(&[0xff00ff]))',6),
    ('ctx.display_module("vega-lite", r#"{"mark":"point","data":{"values":[]}}"#)',7),
])
def test_displays(project,expression,kind):
    root,connect=project
    source='use tsugumi::{Context,Input,Output,Result}; fn run(ctx:&Context<\'_>,_:Input<\'_>)->Result<Output>{'+expression+'?;Ok(Output::default())} tsugumi::export!(run);'
    write_project(root,{'show':rust(source)},[])
    c=connect()
    run_id=succeeded(c)
    assert shown(c,run_id,'show').valueType==kind


def test_borrowed_input_cannot_escape_the_call(project):
    root,connect=project
    source='''use tsugumi::{Context,Input,Output,Result};
fn retain(input: Input<'_>) -> Input<'static> { input }
fn run(_: &Context<'_>, _: Input<'_>) -> Result<Output> { Ok(Output::default()) }
tsugumi::export!(run);'''
    write_project(root,{'escape':rust(source)},[])
    c=connect()
    run_id,status=run_and_wait(c,'start',timeout=1800)
    assert status.state==RunState.failed
    logs=''.join(text(c.call('LogRangeRequest',runId=run_id,card=path('escape'),stream=stream,limitBytes=65536).text) for stream in (1,2,4))
    assert 'lifetime may not live long enough' in logs


def test_required_output_is_validated(project):
    root,connect=project
    card=rust('''use tsugumi::{Context,Input,Output,Result};
fn run(_: &Context<'_>, _: Input<'_>) -> Result<Output> { Ok(Output::default()) }
tsugumi::export!(run);''')
    card['outputs']=NUMBER
    write_project(root,{'missing':card},[])
    c=connect()
    _,status=run_and_wait(c,'start',timeout=1800)
    assert status.state==RunState.failed
    assert 'out' in text(status.failure.diagnostic)


def test_ui_scaffold_is_an_executable_card(project):
    root,connect=project
    card=rust((ROOT/'sdk/rust/template/src/lib.rs').read_text())
    card['outputs']='namespace hello; table Text {value:string;} table Outputs {out:Text(required);} root_type Outputs;'
    write_project(root,{'hello':card},[])
    c=connect()
    run_id=succeeded(c)
    assert text(shown(c,run_id,'hello').value.text)=='hello'

VENDOR_HASH = "sha256-GgPGaed/lz1W3mE8NjW6M/8VldZfZZ4YSw0u0cz5hjM="


def card_with_registry_crate(arguments):
    card = rust("""use std::net::TcpStream;
use std::time::Duration;
use tsugumi::{Context, Input, Output, Result};

fn run(ctx: &Context<'_>, _: Input<'_>) -> Result<Output> {
    let target = "1.1.1.1:443".parse().unwrap();
    assert!(TcpStream::connect_timeout(&target, Duration::from_secs(3)).is_err());
    ctx.display_text(itoa::Buffer::new().format(42))?;
    Ok(Output::default())
}
tsugumi::export!(run);""")
    card['src']['Cargo.toml'] += '\nitoa = "=1.0.15"\n'
    lock = Path(__file__).parent / 'fixtures' / 'rust' / 'Cargo.lock'
    card['src']['Cargo.lock'] = lock.read_text()
    card['nix'] = NIX.replace('src = ./src;', f'src = ./src; {arguments}')
    return card


def test_rust_crates_are_pinned_and_the_card_runs_offline(project):
    root, connect = project
    card = card_with_registry_crate(f'lock = ./src/Cargo.lock; hash = "{VENDOR_HASH}";')
    write_project(root, {'rust': card}, [])
    c = connect()
    assert text(shown(c, succeeded(c), 'rust').value.text) == '42'


@pytest.mark.parametrize(('arguments', 'stale', 'message'), [
    ('lock = ./src/Cargo.lock;', False, 'vendor directory `hash` together'),
    ('lock = ./src/Cargo.lock; hash = pkgs.lib.fakeHash;', False, 'hash mismatch'),
    (f'lock = ./src/Cargo.lock; hash = "{VENDOR_HASH}";', True, 'itoa = "=1.0.14"'),
])
def test_rust_rejects_invalid_pins(project, arguments, stale, message):
    root, connect = project
    card = card_with_registry_crate(arguments)
    if stale:
        card['src']['Cargo.toml'] = card['src']['Cargo.toml'].replace('1.0.15', '1.0.14')
    write_project(root, {'rust': card}, [])
    c = connect()
    run_id, status = run_and_wait(c, 'start', timeout=1800)
    log = text(c.call('LogRangeRequest', runId=run_id, card=path('rust'), stream=LogStream.build).text)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.nix_failed, log
    assert message in text(status.failure.diagnostic) + log


@pytest.mark.parametrize('language', ['ja', 'en'])
def test_bundled_rust_pipeline_runs_in_the_sandbox(project, language):
    root, connect = project
    shutil.copytree(ROOT / 'demos' / language / 'rust-pipeline', root, dirs_exist_ok=True)
    c = connect()
    run_id = succeeded(c)
    assert text(shown(c, run_id, 'check').value.text) == '21'
