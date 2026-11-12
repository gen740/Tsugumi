"""Build the Ruby adapter's port map; Ruby needs no flatc language generator."""
import json
from pathlib import Path
import sys

contract = json.loads(Path(sys.argv[1]).read_text())
out = Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
lines = ['// Generated Ruby port map. Do not edit.', '#pragma once',
         'struct RubyPort { const char* name; const char* root; };']
for kind in ('inputs', 'outputs'):
    lines.append(f'static constexpr RubyPort ruby_{kind}[] = {{')
    lines += ['  {' + json.dumps(p['name']) + ', ' + json.dumps(p['type']) + '},' for p in contract[kind]]
    lines += ['  {nullptr, nullptr}', '};']
(out / 'ruby_contract.hh').write_text('\n'.join(lines) + '\n')
