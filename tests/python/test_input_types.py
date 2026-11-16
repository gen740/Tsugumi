"""Only input_types: compact descriptions versus real SDK generators and parsers.

TSUGUMI_INPUT_TYPES_TESTS selects the small C++ test/description executable.
Missing optional language runtimes skip only that language's parse check.
"""
import ast
import importlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import types
import typing

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = '''namespace sample;
enum Color:byte { Red, Blue }
struct Point { x:double; y:double; }
table Child { item_count:int; }
union Choice { Child }
table Message {
  noise_sd:double;
  title:string;
  children:[Child];
  child:Child;
  flag:bool;
  values:[float];
  color:Color;
  choice:Choice;
  optional_int:int = null;
  point:Point;
}
root_type Message;
'''


def run(args, **kwargs):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, **kwargs)
    assert result.returncode == 0, result.stderr + result.stdout
    return result.stdout


@pytest.fixture
def description(tmp_path):
    binary = Path(os.environ.get('TSUGUMI_INPUT_TYPES_TESTS',
        Path(os.environ.get('TSUGUMI_BUILD_DIR', ROOT / 'build')) / 'tests/core/tsugumi_input_types_tests'))
    if not binary.is_file():
        pytest.skip('build the tsugumi_input_types_tests target')
    schema = tmp_path / 'input.fbs'
    schema.write_text(SCHEMA)

    def render(language):
        before = set(tmp_path.rglob('*'))
        text = run([binary, '--describe', language, schema], cwd=tmp_path)
        assert set(tmp_path.rglob('*')) == before, 'a read tool must not write namespace directories'
        return text
    return render


@pytest.fixture
def generated(tmp_path):
    flatc = shutil.which(os.environ.get('TSUGUMI_FLATC', 'flatc'))
    if not flatc:
        pytest.skip('flatc is required')
    schema = tmp_path / 'input.fbs'
    schema.write_text(SCHEMA)
    run([flatc, '--cpp', '--python', '--go', '--rust', '--csharp', '--gen-object-api',
         '--scoped-enums', '--cs-global-alias', '-o', tmp_path, schema])
    return tmp_path, Path(flatc).resolve().parents[1] / 'include'


def test_python_fields_and_actual_unpacked_types(description, generated, monkeypatch):
    root, _ = generated
    text = description('python')
    ast.parse(text)
    module = types.ModuleType('input_description')
    monkeypatch.setitem(sys.modules, module.__name__, module)
    exec(compile(text, '<input_types>', 'exec'), module.__dict__)
    monkeypatch.syspath_prepend(str(root))
    # flatc's generated modules are the SDK Python face, without a mock mapper.
    for name in ('sample', 'sample.Child', 'sample.Message'):
        monkeypatch.delitem(sys.modules, name, raising=False)
    ChildT = importlib.import_module('sample.Child').ChildT
    MessageT = importlib.import_module('sample.Message').MessageT
    native = MessageT()
    child = ChildT()
    child.itemCount = 7
    native.noiseSd = 0.5
    native.title = 'text'
    native.children = [child]
    native.child = child
    native.values = [1.25, 2.5]
    native.choiceType = 1
    native.choice = child
    native.optionalInt = 2
    import flatbuffers
    builder = flatbuffers.Builder(0)
    builder.Finish(native.Pack(builder))
    Message = importlib.import_module('sample.Message').Message
    received = MessageT.InitFromObj(Message.GetRootAsMessage(builder.Output(), 0))
    hints = {name: eval(annotation, module.__dict__) for name, annotation in module.MessageT.__annotations__.items()
             if name not in ('values',)}
    assert set(module.MessageT.__annotations__) == set(vars(received))
    assert hints['noiseSd'] is float
    assert hints['title'] == typing.Optional[bytes]
    assert type(received.title) is bytes
    assert hints['optionalInt'] == typing.Optional[int]
    assert 'list[float]' in text
    assert 'numpy.ndarray' in text
    assert 'itemCount: int' in text
    assert received.children[0].itemCount == 7
    assert module.MessageT.__dataclass_fields__['children'].name == 'children'
    # Real names, not a snake_case translation of the .fbs.
    assert 'noise_sd:' not in text
    assert 'def Pack' not in text and 'def Init' not in text


def test_cpp_members_compile_and_match_generated_native_objects(description, generated, tmp_path):
    root, include = generated
    compiler = shutil.which(os.environ.get('CXX', 'c++'))
    if not compiler:
        pytest.skip('C++ compiler is required')
    text = description('cpp')
    expected = (root / 'input_generated.h').read_text()
    members = ['double noise_sd = 0.0;', 'std::string title{};',
               'std::vector<std::unique_ptr<sample::ChildT>> children{};',
               'std::unique_ptr<sample::ChildT> child{};', 'bool flag = false;',
               'std::vector<float> values{};', 'sample::Color color = sample::Color::Red;',
               'sample::ChoiceUnion choice{};',
               '::flatbuffers::Optional<int32_t> optional_int = ::flatbuffers::nullopt;']
    for member in members:
        assert member in expected
        assert member in text
    assert 'Pack(' not in text and 'FlatBufferBuilder' not in text
    source = tmp_path / 'description.cc'
    source.write_text(text)
    run([compiler, '-std=c++20', '-fsyntax-only', '-I' + str(include), source])
    # Also typecheck the actual generated counterpart used by the SDK.
    source.write_text('#include "input_generated.h"\n#include <type_traits>\n'
                      'static_assert(std::is_same_v<decltype(sample::MessageT::noise_sd), double>);\n'
                      'static_assert(std::is_same_v<decltype(sample::MessageT::title), std::string>);\n')
    run([compiler, '-std=c++20', '-fsyntax-only', '-I' + str(include), '-I' + str(root), source])


@pytest.mark.parametrize('language,filename', [('go', 'Message.go'), ('rust', None), ('csharp', 'Message.cs')])
def test_other_typed_faces_match_generated_fields(description, generated, language, filename):
    root, _ = generated
    text = description(language)
    expected = (root / ('sample/' + filename if filename else 'input_generated.rs')).read_text()
    if language == 'go':
        body = re.search(r'type MessageT struct \{(.*?)\n\}', expected, re.S)[1]
        assert body.strip() in text
    elif language == 'csharp':
        for field in re.findall(r'public [^\n]+ \{ get; set; \}', expected):
            # The SDK's namespace escaping is applied after flatc.
            field = re.sub(r'global::([\w.]+)', lambda m: 'global::' + '.'.join('@' + x for x in m[1].split('.')), field)
            assert field in text
    else:
        for name in ('noise_sd', 'title', 'children', 'child', 'flag', 'values', 'color', 'choice', 'optional_int'):
            signature = re.search(r'pub fn ' + name + r'\(&self\) -> ([^\n]+) \{', expected)[1]
            assert f'fn {name}(&self) -> {signature};' in text
    assert 'Pack(' not in text and 'FlatBufferBuilder' not in text


@pytest.mark.parametrize('language', ['go', 'rust', 'csharp', 'r', 'julia', 'ruby', 'bash'])
def test_each_language_parses_with_its_own_parser(description, tmp_path, language):
    text = description(language)
    extension = dict(go='go', rust='rs', csharp='cs', r='R', julia='jl', ruby='rb', bash='sh')[language]
    source = tmp_path / ('input.' + extension)
    source.write_text(text)
    parser = dict(go='gofmt', rust='rustfmt', csharp='dotnet', r='Rscript', julia='julia', ruby='ruby', bash='bash')[language]
    command = shutil.which(parser)
    if not command:
        pytest.skip(f'{parser} is not installed')
    if language == 'go':
        run([command, source])
    elif language == 'rust':
        run([command, '--emit', 'stdout', source])
    elif language == 'csharp':
        run([command, 'new', 'classlib', '--no-restore', '-o', tmp_path / 'cs'])
        shutil.copy(source, tmp_path / 'cs/Input.cs')
        run([command, 'build', tmp_path / 'cs', '--nologo'])
    elif language == 'r':
        run([command, '-e', 'parse(file=commandArgs(TRUE)[1])', source])
    elif language == 'julia':
        run([command, '--startup-file=no', '-e', 'Meta.parseall(read(ARGS[1], String))', source])
    elif language == 'ruby':
        run([command, '-c', source])
    else:
        run([command, '-n', source])
        schema = json.loads(run([command, source]))
        assert schema['properties']['noise_sd']['type'] == 'number'
        assert schema['properties']['children']['items']['properties']['item_count']['type'] == 'integer'


def test_json_faces_keep_sdk_field_names(description):
    # JSON-face SDKs preserve schema keys; they do not use flatc's Python/Go names.
    for language in ('r', 'julia', 'ruby', 'bash'):
        text = description(language)
        assert 'noise_sd' in text and 'item_count' in text
        assert 'noiseSd' not in text and 'NoiseSd' not in text
