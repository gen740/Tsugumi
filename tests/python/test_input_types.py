"""Only input_types: compact descriptions versus real SDK generators and parsers.

TSUGUMI_INPUT_TYPES_TESTS selects the small C++ test/description executable.
Missing optional language runtimes skip only that language's parse check.
"""
import ast
import importlib
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
    assert list(map(float, received.values)) == [1.25, 2.5]
    assert eval(module.MessageT.sample__ChildT.__annotations__["itemCount"], module.__dict__) is int
    assert received.children[0].itemCount == 7
    assert module.MessageT.__dataclass_fields__['children'].name == 'children'
    # Real names, not a snake_case translation of the .fbs.


def test_cpp_members_compile_and_match_generated_native_objects(description, generated, tmp_path):
    root, include = generated
    compiler = shutil.which(os.environ.get('CXX', 'c++'))
    if not compiler:
        pytest.skip('C++ compiler is required')
    text = description('cpp')
    expected = (root / 'input_generated.h').read_text()
    def fields(source):
        body = re.search(r'struct\s+MessageT\s*(?::[^{]+)?\{(.*?)\n\};', source, re.S)[1]
        return {name: re.sub(r'\s+', '', kind) for kind, name in re.findall(
            r'^\s*([^;()]+?)\s+(\w+)\s*(?:=\s*[^;]*|\{\})?;', body, re.M) if not kind.strip().startswith('typedef')}
    assert fields(text) == fields(expected)
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
    compact = lambda kind: re.sub(r'\s+|@', '', kind)
    if language == 'go':
        def fields(source):
            body = re.search(r'type\s+MessageT\s+struct\s*\{(.*?)\n\}', source, re.S)[1]
            return {name: compact(kind) for name, kind in re.findall(r'^\s*(\w+)\s+([^\n]+)', body, re.M)}
    elif language == 'csharp':
        def fields(source):
            body = re.search(r'class\s+MessageT\b[^{]*\{(.*?)^\s*\}', source, re.S | re.M)[1]
            return {name: compact(kind) for kind, name in re.findall(r'public\s+([^{};]+?)\s+(\w+)\s*\{\s*get\s*;\s*set\s*;\s*\}', body)}
    else:
        def fields(source):
            return {name: compact(kind) for name, kind in re.findall(r'\bfn\s+(\w+)\s*\(\s*&self\s*\)\s*->\s*([^;{\n]+)', source)}
    described = fields(text)
    actual = fields(expected)
    assert described
    assert described.items() <= actual.items()


def test_json_faces_keep_sdk_field_names(description):
    # JSON-face SDKs preserve schema keys; they do not use flatc's Python/Go names.
    for language in ('r', 'julia', 'ruby', 'bash'):
        text = description(language)
        assert 'noise_sd' in text and 'item_count' in text
        assert 'noiseSd' not in text and 'NoiseSd' not in text
