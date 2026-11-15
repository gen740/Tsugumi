"""Keep the host folder's single format number tied to every persisted schema.

All CREATE statements under host/ and agent/ belong to databases in host-core,
or to conversation tables shared with the per-project Agent database. Discover
sources rather than maintaining a list that could silently miss a new database.
"""
import ast
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
# Consume comments as tokens as well, so quoted examples in comments are ignored.
TOKENS = re.compile(
    r"'(?:\\.|[^'\\])*'|"
    r'//[^\n]*|/\*.*?\*/|R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\('
    r'(?P<raw>.*?)\)(?P=delimiter)"|"(?:\\.|[^"\\])*"',
    re.DOTALL,
)
CREATE = re.compile(r'\bCREATE\s+(?:TABLE|(?:UNIQUE\s+)?INDEX)\b[^;]*', re.I)


def schema_digest():
    statements = []
    for component in ("host", "agent"):
        for source in sorted((ROOT / "core/src" / component).rglob("*")):
            if source.suffix not in {".cc", ".hh"}:
                continue
            text = source.read_text()
            literals = []
            previous_end = -1
            for token in TOKENS.finditer(text):
                if token[0].startswith(("//", "/*", "'")):
                    continue
                value = token["raw"] if token["raw"] is not None else ast.literal_eval(token[0])
                if literals and not text[previous_end:token.start()].strip():
                    literals[-1] += value
                else:
                    literals.append(value)
                previous_end = token.end()
            for literal in literals:
                for statement in CREATE.findall(literal):
                    statements.append(f"{source.relative_to(ROOT)}\n{' '.join(statement.split())}\n")
    assert statements, "No host state schema statements found"
    return hashlib.sha256("".join(statements).encode()).hexdigest()


def test_host_state_format_covers_all_database_definitions():
    source = (ROOT / "core/src/host/state_format.hh").read_text()
    number = re.search(r'kStateFormat\s*=\s*(\d+);', source)
    recorded = re.search(r'kStateSchemaDigest\s*=\s*"([0-9a-f]{64})";', source)
    actual = schema_digest()
    assert number and int(number[1]) > 0
    assert recorded and recorded[1] == actual, (
        "Host state tables or indexes changed. Raise kStateFormat and record "
        f'the new kStateSchemaDigest in host/state_format.hh: {actual}'
    )
