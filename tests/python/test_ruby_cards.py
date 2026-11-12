"""MRI package Cards use the existing ABI, with typed ports and resident state."""
import json
import os
import shutil
import time

import pytest

from conftest import ROOT, Daemon, real_environment
from projects import run_and_wait, start_run, wait_run
from test_language_cards import project, write_project
from tsugumi_client import path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState

IN_VM = os.environ.get("TSUGUMI_TEST_CONTAINER") == "1"
RUBY_NIX = '{ pkgs, inputs, params }: let sdk = inputs.tsugumi.sdk { languages = [ "ruby" ]; }; in { run = inputs.tsugumi.rubyCard { inherit sdk; src = ./src; }; }'
EMPTY = 'namespace test; table Outputs {} root_type Outputs;'
NUMBERS = '''attribute "tsugumi";
namespace sample;
table Number { value:ulong; tiny:double; label:string; bytes:[ubyte]; }
table Outputs { out:Number (required); }
root_type Outputs;'''


@pytest.fixture
def ruby_project(tmp_path):
    root = tmp_path / 'ruby'
    environments = []
    def connect(cards, connections=()):
        write_project(root, cards, list(connections))
        # Exercise Seatbelt on a macOS host, bubblewrap in the VM/Linux job.
        for file in root.glob('definitions/*/_card.json'):
            data = json.loads(file.read_text())
            data.pop('sandbox', None)
            file.write_text(json.dumps(data))
        env = real_environment(root, 'tsugumi-test-ruby') if IN_VM else Daemon(root).start()
        environments.append(env)
        client = env.client()
        if not IN_VM:
            client.hello()
        return client
    yield root, connect
    for env in environments:
        env.cleanup()


def ruby(source, *, params=(), outputs=EMPTY, **settings):
    return {'card': {'language': 'ruby', 'params': list(params), **settings}, 'nix': RUBY_NIX,
            'outputs': outputs, 'src': {'main.rb': source}}


def shown(client, run, card='ruby'):
    return client.call('RunRecordRequest', runId=run, card=path(card)).record.display.value


def test_ruby_resident_state_gc_and_run_start(ruby_project):
    _, connect = ruby_project
    c = connect({'ruby': ruby('''
def on_run_start(ctx)
  @runs = (@runs || 0) + 1
end

def main(ctx)
  GC.start
  @calls = (@calls || 0) + 1
  raise 'params differ' unless ctx.params['offset'] == 9007199254740993
  ctx.log("run=#{@runs} call=#{@calls}")
  ctx.display_table(['runs', 'calls', 'param'], [[@runs, @calls, ctx.params['offset']]])
end
''', params=[{'name':'offset', 'type':'uint64', 'data':9007199254740993}], reuseWorker=True)})
    for expected in [1, 2]:
        rid, status = run_and_wait(c, 'ruby', timeout=240)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        table = shown(c, rid)
        assert [text(x) for x in table.rows[0].cells][:2] == [str(expected), str(expected)]
        assert text(table.rows[0].cells[2]) == '9007199254740993'
        logs = c.call('LogRangeRequest', runId=rid, card=path('ruby'), invocation=1, stream=2)
        assert f'run={expected} call={expected}' in text(logs.text)


def test_ruby_no_reuse_starts_with_fresh_state(ruby_project):
    _, connect = ruby_project
    c = connect({'ruby': ruby('def main(ctx); @n = (@n || 0) + 1; ctx.display_scalar(@n, "counter"); end', reuseWorker=False)})
    for _ in range(2):
        rid, status = run_and_wait(c, 'ruby', timeout=240)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        assert shown(c, rid).value == 1


def test_python_ruby_cpp_preserve_typed_values_and_binary(ruby_project):
    _, connect = ruby_project
    source = '''import outputs.out.sample.Number as number

def main(ctx):
    b = ctx.builder()
    label = b.CreateString("hello 日本語")
    raw = b.CreateByteVector(bytes([0, 127, 255]))
    number.Start(b)
    number.AddValue(b, 18446744073709551615)
    number.AddTiny(b, 1e-20)
    number.AddLabel(b, label)
    number.AddBytes(b, raw)
    ctx.output("out", b, number.End(b))
'''
    transform = '''Message = Struct.new(:value, :tiny, :label, :bytes, keyword_init: true)
def main(ctx)
  input = ctx.input('in')
  raise 'integer changed' unless input['value'] == 18446744073709551615
  raise 'float changed' unless input['tiny'] == 1e-20
  raise 'bytes changed' unless input['bytes'] == [0, 127, 255]
  ctx.output('out', Message.new(value: input['value'], tiny: input['tiny'], label: input['label'].upcase,
                              bytes: Tsugumi.bytes("\\x00\\x7f\\xff".b)))
  ctx.display_text(input['label'])
end
'''
    sink = '''#include <card.hh>
#include <stdexcept>
auto run(Card::Input& input) -> Card::Output {
  if (input.in.value != UINT64_MAX || input.in.tiny != 1e-20 || input.in.bytes.size() != 3 || input.in.bytes[2] != 255)
    throw std::runtime_error("Ruby output changed");
  tsugumi::visualization::text(input.in.label);
  return {};
}
'''
    c = connect({
        'python': {'card': {'language':'python'}, 'nix': '{ pkgs, inputs, params }: let sdk = inputs.tsugumi.sdk { languages = [ "python" ]; }; in { run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; }; }', 'outputs':NUMBERS, 'src':{'main.py':source}},
        'ruby': ruby(transform, inputs=[{'name':'in'}], outputs=NUMBERS),
        'cpp': {'card': {'language':'cpp','inputs':[{'name':'in'}]}, 'nix': '{ pkgs, inputs, params }: let sdk = inputs.tsugumi.sdk { languages = [ "cpp" ]; }; in { run = inputs.tsugumi.cppCard { inherit sdk; src = ./src; }; }', 'outputs':EMPTY, 'src':{'card.cc':sink}},
    }, [('python','out','ruby','in'), ('ruby','out','cpp','in')])
    rid, status = run_and_wait(c, 'python', timeout=300)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert text(shown(c, rid, 'cpp').text) == 'HELLO 日本語'


def test_ruby_files_directories_and_context_lifetime(ruby_project):
    _, connect = ruby_project
    files = '''attribute "tsugumi"; namespace f; table Path { value:string; }
      table Outputs { file:Path (required, tsugumi:"file"); directory:Path (required, tsugumi:"directory"); }
      root_type Outputs;'''
    produce = '''def main(ctx)
  if @previous
    begin
      @previous.log('expired')
      raise 'expired context was accepted'
    rescue Tsugumi::Failure => e
      raise unless e.message.include?('expired')
    end
  end
  @previous = ctx.dup
  error = Thread.new { begin; ctx.params; rescue Tsugumi::Failure => e; e.message; end }.value
  raise 'cross-thread context accepted' unless error.include?('thread')
  file = File.join(ctx.output_directory, 'report.txt')
  directory = File.join(ctx.output_directory, 'bundle')
  Dir.mkdir(directory)
  File.write(file, '日本語')
  File.write(File.join(directory, 'value.txt'), 'directory')
  ctx.output('file', value: file)
  ctx.output('directory', value: directory)
end'''
    consume = '''def main(ctx)
  raise 'file changed' unless File.read(ctx.path('file')) == '日本語'
  raise 'directory changed' unless File.read(File.join(ctx.path('directory'), 'value.txt')) == 'directory'
  raise 'unexpected optional input' unless ctx.input('optional').nil?
  ctx.display_markdown('## Read both artifacts')
end'''
    c = connect({'ruby': ruby(produce, outputs=files),
                 'read': ruby(consume, inputs=[{'name':'file'}, {'name':'directory'}, {'name':'optional','optional':True}])},
                [('ruby','file','read','file'), ('ruby','directory','read','directory')])
    for _ in range(2):
        rid, status = run_and_wait(c, 'ruby', timeout=300)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        assert text(shown(c, rid, 'read').text) == '## Read both artifacts'


@pytest.mark.parametrize('expression,diagnostic', [
    ("ctx.output('out', nil)", 'omit the call'),
    ("ctx.output('out', value: 18446744073709551616)", 'does not fit'),
    ("ctx.output('out', label: \"\\xff\".b)", 'UTF-8'),
    ("ctx.output('out', value: 'wrong')", 'enum values'),
    ("ctx.fail('worker_failed', 'specific failure')", 'specific failure'),
    ("Thread.new { sleep 60 }", 'join Ruby threads'),
])
def test_ruby_failures_are_structured_and_next_worker_is_fresh(ruby_project, expression, diagnostic):
    root, connect = ruby_project
    source = f'''def main(ctx)
  @calls = (@calls || 0) + 1
  raise 'failed worker was reused' unless @calls == 1
  {expression}
end'''
    c = connect({'ruby': ruby(source, outputs=NUMBERS), 'good': ruby("def main(ctx); ctx.display_text('alive'); end")})
    for _ in range(2):
        rid, status = run_and_wait(c, 'ruby', timeout=300)
        assert status.state == RunState.failed
        record = c.call('RunRecordRequest', runId=rid, card=path('ruby')).record
        assert diagnostic in text(record.summary.failure.diagnostic), text(record.summary.failure.diagnostic)
    rid, status = run_and_wait(c, 'good', timeout=300)
    assert status.state == RunState.succeeded
    assert text(shown(c, rid, 'good').text) == 'alive'


@pytest.mark.parametrize('cancel', [False, True])
def test_ruby_deadline_and_cancel_leave_next_run_working(ruby_project, cancel):
    _, connect = ruby_project
    c = connect({'ruby': ruby("def main(ctx); ctx.log('ready'); loop { sleep 0.01 }; end", timeoutSeconds=60 if cancel else 1),
                 'good': ruby("def main(ctx); ctx.display_text('alive'); end")})
    rid = start_run(c, 'ruby')
    if cancel:
        deadline = time.monotonic() + 300
        while time.monotonic() < deadline:
            logs = c.call('LogRangeRequest', runId=rid, card=path('ruby'), invocation=1, stream=2)
            if 'ready' in text(logs.text):
                break
            time.sleep(0.1)
        else:
            pytest.fail('Ruby never started')
        c.call('RunCancelRequest', runId=rid)
    status = wait_run(c, rid, timeout=300)
    assert status.state == (RunState.cancelled if cancel else RunState.failed)
    if not cancel:
        record = c.call('RunRecordRequest', runId=rid, card=path('ruby')).record
        assert record.summary.failure.kind == FailureKind.timeout
    _, status = run_and_wait(c, 'good', timeout=300)
    assert status.state == RunState.succeeded


COLORIZE_HASH = "sha256-pINRA2ThhG24fQ1Ewj7K4pmEE/ytgPxSlRQtTKOoLdY="


def colorize_card(arguments, requirement="1.1.0"):
    card = ruby("""require 'colorize'
require 'socket'
def main(ctx)
  spec = Gem.loaded_specs.fetch('colorize')
  raise "gem came from #{spec.full_gem_path}" unless spec.full_gem_path.include?('tsugumi-ruby-card-gems')
  begin
    Socket.tcp('1.1.1.1', 443, connect_timeout: 3).close
  rescue SystemCallError, IOError
    ctx.display_text("offline #{spec.version}")
  else
    raise 'network was available'
  end
end""")
    card['nix'] = RUBY_NIX.replace('src = ./src;', f'src = ./src; {arguments}')
    card['src'].update({
        'gems/Gemfile': f'source "https://rubygems.org"\ngem "colorize", "{requirement}"\n',
        'gems/Gemfile.lock': """GEM
  remote: https://rubygems.org/
  specs:
    colorize (1.1.0)

PLATFORMS
  ruby

DEPENDENCIES
  colorize (= 1.1.0)

BUNDLED WITH
   2.6.9
""",
    })
    return card


def test_ruby_gems_are_locked_and_loaded_without_network(ruby_project):
    _, connect = ruby_project
    c = connect({'ruby': colorize_card(f'lock = ./src/gems/Gemfile.lock; hash = "{COLORIZE_HASH}";')})
    rid, status = run_and_wait(c, 'ruby', timeout=600)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert text(shown(c, rid).text) == 'offline 1.1.0'


@pytest.mark.parametrize(('arguments', 'requirement', 'message'), [
    ('lock = ./src/gems/Gemfile.lock;', "1.1.0", "gems need both"),
    ('lock = ./src/gems/Gemfile.lock; hash = pkgs.lib.fakeHash;', "1.1.0", "hash mismatch"),
    (f'lock = ./src/gems/Gemfile.lock; hash = "{COLORIZE_HASH}";', "1.0.0", "does not match its Gemfile"),
])
def test_ruby_rejects_an_invalid_pin(ruby_project, arguments, requirement, message):
    _, connect = ruby_project
    c = connect({'ruby': colorize_card(arguments, requirement)})
    rid, status = run_and_wait(c, 'ruby', timeout=600)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.nix_failed
    log = c.call('LogRangeRequest', runId=rid, card=path('ruby'), stream=LogStream.build, limitBytes=65536)
    assert message in text(status.failure.diagnostic) + text(log.text)


@pytest.mark.parametrize('expression,kind', [
    ("ctx.display_text('text')", 1),
    ("ctx.display_markdown('**bold**')", 1),
    ("ctx.display_line_chart([{id: 'line', x: [1, 2], y: [2, 4]}])", 3),
    ("ctx.display_points([1], [2], [3], [0xff00ff])", 6),
    ("ctx.display_image(['89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000b49444154789c636000020000050001a5f645400000000049454e44ae426082'].pack('H*'), alt: 'pixel')", 5),
    ("ctx.display_plotly(data: [{x: [1], y: [2], type: 'scatter'}])", 7),
])
def test_ruby_display_bridge_owns_bytes(ruby_project, expression, kind):
    _, connect = ruby_project
    c = connect({'ruby': ruby(f'def main(ctx); {expression}; GC.start; end')})
    rid, status = run_and_wait(c, 'ruby', timeout=300)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    display = c.call('RunRecordRequest', runId=rid, card=path('ruby')).record.display
    assert display.valueType == kind
    if 'display_markdown' in expression:
        assert display.value.markdown
    if kind == 5:
        assert bytes(display.value.data).startswith(b'\x89PNG')
        assert text(display.value.alt) == 'pixel'
    if kind == 7:
        assert json.loads(text(display.value.dataJson))['data'][0]['y'] == [2]

@pytest.mark.parametrize(('source', 'message'), [
    ('def main(ctx)\n', 'syntax error'),
    ('def helper; end\n', 'main'),
])
def test_ruby_load_errors_are_structured(ruby_project, source, message):
    _, connect = ruby_project
    c = connect({'ruby': ruby(source)})
    rid, status = run_and_wait(c, 'ruby', timeout=600)
    assert status.state == RunState.failed and status.failure.kind == FailureKind.worker_failed
    log = c.call('LogRangeRequest', runId=rid, card=path('ruby'), stream=LogStream.stderr, limitBytes=65536)
    assert message in text(status.failure.diagnostic) + text(log.text)


def test_ruby_rejects_an_unknown_output_field(ruby_project):
    _, connect = ruby_project
    card = ruby("def main(ctx); ctx.output('out', {vale: 42}); end", outputs=NUMBERS)
    c = connect({'ruby': card})
    rid, status = run_and_wait(c, 'ruby', timeout=600)
    assert status.state == RunState.failed
    log = c.call('LogRangeRequest', runId=rid, card=path('ruby'), stream=LogStream.stderr, limitBytes=65536)
    assert 'vale' in text(status.failure.diagnostic) + text(log.text)


@pytest.mark.parametrize('language', ['ja', 'en'])
def test_bundled_ruby_pipeline_runs_in_the_sandbox(project, language):
    root, connect = project
    shutil.copytree(ROOT / 'demos' / language / 'ruby-pipeline', root)
    c = connect()
    rid, status = run_and_wait(c, 'start', timeout=1800)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert text(shown(c, rid, 'cpp').text) == 'C++ received 42'
