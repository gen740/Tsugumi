"""Compare the vendored Card ABI with the canonical core declarations.

The ABI is card.fbs, every schema it includes (transitively) and card_abi.h.
The kit vendors exactly these files, under the same names, in abi/.
This includes artifact.fbs, which is also installed for user schemas.
"""

import re
import sys
from pathlib import Path


def uncomment(text):
    return re.sub(r"//[^\n]*|/\*[\s\S]*?\*/", "", text)


def tokens(text):
    return re.findall(r"\w+|[^\s\w]", text)


def header(text):
    text = uncomment(text)
    # The two installed headers may use different include guards.
    text = re.sub(r"\A\s*#ifndef (\w+)\s+#define \1[^\S\n]*\n", "", text)
    return tokens(re.sub(r"\n#endif\s*\Z", "", text))


def schemas(directory, name, seen=None):
    """card.fbs and the closure of its includes, in include order."""
    seen = [] if seen is None else seen
    if name in seen:
        return seen
    seen.append(name)
    for include in re.findall(r'^include "([^"]+)";', (directory / name).read_text(), re.M):
        schemas(directory, include, seen)
    return seen


def main(protocol, kit):
    names = schemas(protocol, "card.fbs")
    assert "artifact.fbs" in names, "the Card ABI must include artifact.fbs"
    assert schemas(kit, "card.fbs") == names, "kit card.fbs includes a different set of schemas"
    vendored = {path.name for path in kit.iterdir()}
    expected = set(names) | {"card_abi.h"}
    assert vendored == expected, f"kit vendors {sorted(vendored)}, expected {sorted(expected)}"
    for name in names:
        assert tokens(uncomment((protocol / name).read_text())) == tokens(
            uncomment((kit / name).read_text())
        ), f"kit {name} differs from the canonical Card ABI"
    assert header((protocol / "include/tsugumi/card_abi.h").read_text()) == header(
        (kit / "card_abi.h").read_text()
    ), "kit card_abi.h differs from the canonical Card ABI"


if __name__ == "__main__":
    main(*(Path(arg) for arg in sys.argv[1:]))
