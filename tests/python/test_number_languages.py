"""Every SDK round-trips the same typed numeric conformance vectors."""
import json
import math
from pathlib import Path
import struct

import flatbuffers.table
import flatbuffers.number_types as flags
import pytest

from test_language_cards import JULIA_NIX, R_NIX, project, write_project
from projects import run_and_wait
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

ROOT = Path(__file__).resolve().parents[2]
VECTORS = json.loads((ROOT / 'tests/fixtures/number-vectors.json').read_text())
SCHEMA = '''namespace numbers;
table Numbers { values:[double]; strings:[string]; signed:long; unsigned:ulong; }
table Outputs { out:Numbers (required); } root_type Outputs;'''

SOURCES = {
    'cpp': ('card.cc', '''#include <card.hh>
auto run(const Card::Inputs& i) -> Card::Output {
  Card::Output out;
  auto& m = out.out.emplace();
  m.values = i.number.values; m.strings = i.number.strings;
  m.signed_ = i.number.signed_; m.unsigned_ = i.number.unsigned_;
  return out;
}'''),
    'python': ('main.py', '''def main(inputs):
  m = inputs.number
  return {'out': {'values': list(m.values), 'strings': list(m.strings), 'signed': m.signed, 'unsigned': m.unsigned}}
'''),
    'ruby': ('main.rb', "def main(inputs); {'out' => inputs.number}; end\n"),
    'r': ('main.R', 'main <- function(inputs) list(out = inputs$number)\n'),
    'julia': ('main.jl', 'main(inputs) = Dict("out" => inputs.number)\n'),
    'go': ('card.go', '''package main
import "tsugumi"
func run(i tsugumi.Inputs)(tsugumi.Output,error) {
  m := i.Number()
  return tsugumi.Output{Out:&tsugumi.OutputOut{Values:m.Values,Strings:m.Strings,Signed:m.Signed,Unsigned:m.Unsigned}},nil
}
func onRunStart(i tsugumi.Inputs)error{return nil}
'''),
    'rust': ('src/lib.rs', '''use tsugumi::{Inputs,Output,Result};
fn run(i:Inputs<'_>) -> Result<Output> {
 let mut m=tsugumi::outputs::Out::default();
 m.values=i.number.values().map(|v|v.iter().collect());
 m.strings=i.number.strings().map(|v|v.iter().map(str::to_string).collect());
 m.signed=i.number.signed();m.unsigned=i.number.unsigned();
 Ok(Output{out:Some(m)})
}
tsugumi::export!(run,inputs);
'''),
    'csharp': ('Card.cs', '''using Tsugumi;
using System.Threading.Tasks;
public sealed class Card:ICard {
 public ValueTask<Outputs> RunAsync(Inputs i) {
  var m=i.Number;
  return ValueTask.FromResult(new Outputs{Out=new outputs.@out.numbers.NumbersT {
   Values=m.Values,Strings=m.Strings,Signed=m.Signed,Unsigned=m.Unsigned}});
 }
}
'''),
}


def sdk_card(language):
    filename, source = SOURCES[language]
    helper = {'r': 'rCard', 'julia': 'juliaCard', 'csharp': 'csharpCard'}.get(language, language + 'Card')
    files = {filename: source}
    if language == 'go':
        files.update({'go.mod': (ROOT / 'sdk/go/template/go.mod').read_text(), 'go.sum': ''})
    if language == 'rust':
        files['Cargo.toml'] = (ROOT / 'sdk/rust/template/Cargo.toml').read_text()
    nix = {'r': R_NIX, 'julia': JULIA_NIX}.get(language,
        '{pkgs,inputs,params}:let sdk=inputs.tsugumi.sdk {languages=["' + language + '"];};in {run=inputs.tsugumi.' + helper + ' {inherit sdk;src=./src;};}')
    return {'card': {'language': language, 'inputs': [{'name': 'number'}], 'recordIO': True},
            'outputs': SCHEMA, 'src': files, 'nix': nix}


@pytest.mark.environment
@pytest.mark.parametrize('language', list(SOURCES))
def test_numeric_meaning_survives_each_sdk(project, language):
    root, connect = project
    make = {'card': {'language': 'python'}, 'outputs': SCHEMA,
            'nix': '{pkgs,inputs,params}:let sdk=inputs.tsugumi.sdk {languages=["python"];};in {run=inputs.tsugumi.pythonCard {inherit sdk;src=./src;};}',
            'src': {'main.py': 'v=' + repr(VECTORS) + '''
def main(inputs):
 return {'out': {'values': [float(x) for x in v['values']], 'strings': v['strings'],
                 'signed': int(v['signed']), 'unsigned': int(v['unsigned'])}}
'''}}
    write_project(root, {'make': make, 'check': sdk_card(language)}, [('make', 'out', 'check', 'number')])
    client = connect()
    run_id, status = run_and_wait(client, 'start', timeout=1800)
    assert status.state == RunState.succeeded, (status.failure and text(status.failure.diagnostic),
        [(card, stream, text(client.call('LogRangeRequest', runId=run_id, card=path(card),
            stream=stream, limitBytes=65536).text)) for card in ('make', 'check') for stream in (2, 4)])
    value = client.call('RunRecordRequest', runId=run_id, card=path('check')).record.outputs[0]
    data = bytes(value.inlineBytes)
    table = flatbuffers.table.Table(bytearray(data), struct.unpack_from('<I', data)[0])
    offset = table.Offset(4)
    actual = [table.Get(flags.Float64Flags, table.Vector(offset) + i * 8) for i in range(table.VectorLen(offset))]
    for got, token in zip(actual, VECTORS['values'], strict=True):
        expected = float(token)
        assert math.isnan(got) if math.isnan(expected) else struct.pack('<d', got) == struct.pack('<d', expected)
    offset = table.Offset(6)
    assert [table.String(table.Vector(offset) + i * 4).decode() for i in range(table.VectorLen(offset))] == VECTORS['strings']
    assert table.Get(flags.Int64Flags, table.Pos + table.Offset(8)) == int(VECTORS['signed'])
    assert table.Get(flags.Uint64Flags, table.Pos + table.Offset(10)) == int(VECTORS['unsigned'])
