"""Parse code embedded in the Agent prompt; prose is free to change."""
import ast
import os
from pathlib import Path
import re
import subprocess
import textwrap

ROOT = Path(__file__).resolve().parents[2]
PROMPT = ROOT / 'hostd/agent/Shared/system-prompt.md'


def test_flatbuffer_examples_compile(tmp_path):
    source = PROMPT.read_text()
    for index, match in enumerate(re.finditer(r'^ +attribute "tsugumi";\n(?: +.*\n)*? +root_type Outputs;', source, re.M)):
        schema = tmp_path / f'example{index}.fbs'
        schema.write_text(textwrap.dedent(match[0]))
        subprocess.run([os.environ.get('TSUGUMI_FLATC', 'flatc'), '--python', '--gen-object-api',
                        '-o', str(tmp_path), str(schema)], check=True, capture_output=True, text=True)


def test_python_examples_parse_and_use_existing_sdk_members():
    sdk = ast.parse((ROOT / 'sdk/python/tsugumi/__init__.py').read_text())
    members = {role.lower(): {node.name for node in cls.body if isinstance(node, ast.FunctionDef)}
               for cls in sdk.body if isinstance(cls, ast.ClassDef) and (role := cls.name) in ('Report', 'Run')}
    for match in re.finditer(r'^ {8}def main\([^\n]*\n(?: {8,}[^\n]*\n|\n)*', PROMPT.read_text(), re.M):
        tree = ast.parse(textwrap.dedent(match[0]))
        compile(tree, str(PROMPT), 'exec')
        for node in ast.walk(tree):
            if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) and node.value.id in members:
                assert node.attr in members[node.value.id], f'{node.value.id}.{node.attr} is not in the SDK'


def test_nix_examples_parse(tmp_path):
    for index, match in enumerate(re.finditer(r'^ {6}\{ pkgs, inputs, params \}:\n(?: {6,}[^\n]*\n|\n)*?^ {6}\}', PROMPT.read_text(), re.M)):
        expression = tmp_path / f'example{index}.nix'
        expression.write_text(textwrap.dedent(match[0]))
        subprocess.run(['nix-instantiate', '--parse', str(expression)], check=True, capture_output=True, text=True)
