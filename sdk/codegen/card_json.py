"""Build the port map of a Card's JSON face (sdk/src/json_face.cc): which root
type each port carries. Languages on this face need no flatc generator."""
import json
from pathlib import Path
import sys

contract = json.loads(Path(sys.argv[1]).read_text())
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
lines = ['// Generated port map of the JSON face. Do not edit.', '#pragma once',
         'struct JsonPort { const char* name; const char* root; };']
for kind in ('inputs', 'outputs'):
    lines.append(f'static constexpr JsonPort json_{kind}[] = {{')
    lines += ['  {' + json.dumps(p['name']) + ', ' + json.dumps(p['type']) + '},' for p in contract[kind]]
    lines += ['  {nullptr, nullptr}', '};']
(out / 'json_contract.hh').write_text('\n'.join(lines) + '\n')
