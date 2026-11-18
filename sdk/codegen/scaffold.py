"""Synchronize a host language scaffold from its SDK template."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def generate(language, check=False):
    template_root = ROOT / f'sdk/{language}/template'
    files = {str(p.relative_to(template_root)): p.read_text() for p in sorted(template_root.rglob('*')) if p.is_file()}
    path = ROOT / f'ui/shared/templates/{language}.json'
    template = json.loads(path.read_text())
    template = {name: text for name, text in template.items() if not name.startswith('src/')}
    template.update({'src/' + name: text for name, text in files.items()})
    outputs = {path: json.dumps(template, ensure_ascii=False, indent=2) + '\n'}
    for path, value in outputs.items():
        if check:
            if not path.exists() or path.read_text() != value:
                raise SystemExit(f'Stale scaffold: {path}')
        else:
            path.write_text(value)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('language', choices=['go', 'rust'])
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    generate(args.language, args.check)
