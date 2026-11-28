"""Record the DDL associated with each persistent SQLite format version."""

from hashlib import sha256
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
AGENT = "hostd/agent/src/store.cc"
STORES = {
    "host.sqlite": (
        "hostd/private/host/store.hh", "kHostFormatVersion", ["hostd/src/store.cc", AGENT],
    ),
    "agent.sqlite": (
        "hostd/agent/private/agent/store.hh", "kAgentFormatVersion", [AGENT],
    ),
    "execution.sqlite": (
        "hostd/private/host/execution_broker.hh", "kExecutionFormatVersion", ["hostd/src/execution_broker.cc"],
    ),
    "document.sqlite": (
        "tsugumid/runtime/src/store.cc", "kDocumentFormatVersion", ["tsugumid/runtime/src/store.cc"],
    ),
    "runs.sqlite": (
        "tsugumid/runtime/src/history.cc", "kHistoryVersion", ["tsugumid/runtime/src/history.cc"],
    ),
}

# Keep existing entries. When the DDL changes, bump the version in C++ and add
# the new fingerprint reported by the test. Shared Agent tables affect both stores.
FORMATS = {
    "host.sqlite": {
        1: "a62f2f02b16d73e06865a238a815cf2a4d124288bd24d807a3b0b07a0c118330",
        2: "fd97e56cb6a37ae96b2e0a0f1e0ed0faced8fc370ef16f0d4a59469eba096840",
    },
    "agent.sqlite": {1: "b1e75f930be0b85116bdf504523e61000d8d6ab397ba61979c7b39328044320f"},
    "execution.sqlite": {
        1: "c96812aecbd05d3aab28c807d6c97d9dd35b6ff91ae128f456e54101e0ba50bf",
        2: "8b1f18bf86da9012eb7a3b741b89e07e0052f70eaea5b10b0dd13af81299cae3",
    },
    "document.sqlite": {1: "30a8e2ec2db7a1145734129946c14e39d3dc2e65be98eaf8d9a803ad13ba0b8e"},
    "runs.sqlite": {24: "b93564dad1d518cf7845be2aaac4e6e579e9d0f39121cd1090cae6aede01345d"},
}

# The initializers use adjacent C++ string literals and raw SQL strings. The
# history initializer also interpolates a schema prefix between literals.
LITERAL = re.compile(r'R"(?P<tag>sql|SQL)\((?P<raw>.*?)\)(?P=tag)"|"(?P<quoted>[^"\n]*)"', re.S)
DDL = re.compile(r"\b(?:CREATE|ALTER|DROP)\s+(?:(?:UNIQUE)\s+)?(?:TABLE|INDEX|VIEW|TRIGGER)\b[^;]*")


def schema(source):
    # Preserve spacing inside the SQL while ignoring source indentation and
    # C++ string wrapping. A boundary separates independent SQL calls.
    parts = []
    last = 0
    for match in LITERAL.finditer(source):
        between = source[last:match.start()].strip()
        if between and not re.fullmatch(r"\+\s*p\s*\+", between):
            parts.append(";")
        parts.append(match["raw"] if match["raw"] is not None else match["quoted"])
        last = match.end()
    return [" ".join(statement.split()) for statement in DDL.findall("".join(parts))]


def test_sqlite_schema_formats():
    for name, (version_file, constant, sources) in STORES.items():
        version = int(re.search(rf"\b{constant} = (\d+);", (ROOT / version_file).read_text())[1])
        statements = [ddl for path in sources for ddl in schema((ROOT / path).read_text())]
        assert statements, f"No DDL found for {name}"
        digest = sha256("\n".join(statements).encode()).hexdigest()
        assert FORMATS[name].get(version) == digest, (
            f"{name} format {version} has DDL fingerprint {digest}. "
            f"For a schema change, bump {constant} and record the new fingerprint."
        )


def test_persistent_sqlite_ddl_is_covered():
    covered = {path for _, _, paths in STORES.values() for path in paths}
    for directory in ("hostd", "tsugumid", "util"):
        for source in (ROOT / directory).rglob("*.cc"):
            path = source.relative_to(ROOT).as_posix()
            assert path in covered or not schema(source.read_text()), (
                f"Register the SQLite DDL in {path} with its store's format check"
            )
