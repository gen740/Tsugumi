"""Generate the C# Card scaffold shared by the Mac and Web creation menus."""
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
FILES = {
    '_card.json': json.dumps({'language': 'csharp', 'inputs': [{'name': 'in'}],
                             'params': [{'name': 'message', 'type': 'string', 'data': 'Hello from C#'}]}, indent=2) + '''
''',
    '_outputs.fbs': '''namespace card;
table Outputs {}
root_type Outputs;
''',
    'default.nix': '''{ pkgs, inputs, params }:
let
  sdk = inputs.tsugumi.sdk { languages = [ "csharp" ]; };
in
{
  run = inputs.tsugumi.csharpCard { inherit sdk; src = ./src; };
}
''',
    'src/Card.cs': '''using System.Threading.Tasks;
using Tsugumi;

public sealed class Card : ICard
{
    public ValueTask<Outputs> RunAsync(Inputs inputs, Report report)
    {
        var result = new Outputs();
        report.Text(inputs.Params.Message);
        return ValueTask.FromResult(result);
    }
}
''',
}

data = json.dumps(FILES, ensure_ascii=False, indent=2) + '''
'''
for path, content in [(ROOT / 'ui/shared/templates/csharp.json', data)]:
    if '--check' in sys.argv:
        if not path.exists() or path.read_text() != content:
            raise SystemExit(f'{path} is stale')
    else:
        path.write_text(content)
