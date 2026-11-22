"""Compare the vendored Card ABI with the canonical core declarations."""

import re
import sys
from pathlib import Path


def uncomment(text):
    return re.sub(r"//[^\n]*|/\*[\s\S]*?\*/", "", text)


def tokens(text):
    return re.findall(r"\w+|[^\s\w]", text)


def header(text):
    text = uncomment(text)
    # The two installed headers use different include paths and guards.
    text = re.sub(r"\A\s*#ifndef (\w+)\s+#define \1[^\S\n]*\n", "", text)
    return tokens(re.sub(r"\n#endif\s*\Z", "", text))


def enum(text, name):
    declaration = re.search(rf"\benum\s+{name}\s*:[^{{]+\{{[^}}]+\}}", uncomment(text))
    assert declaration is not None, f"missing Card ABI enum {name}"
    return tokens(declaration[0])


def main(protocol, kit):
    assert tokens(uncomment((protocol / "card.fbs").read_text())) == tokens(
        uncomment((kit / "card.fbs").read_text())
    ), "kit card.fbs differs from the canonical Card ABI"
    assert header((protocol / "include/tsugumi/card_abi.h").read_text()) == header(
        (kit / "abi.h").read_text()
    ), "kit abi.h differs from the canonical Card ABI"
    # card.fbs uses these two enums. The shared file also contains RPC enums.
    core_enums = (protocol / "tsugumi_enums.fbs").read_text()
    kit_enums = (kit / "tsugumi_enums.fbs").read_text()
    for name in ("PortKind", "FailureKind"):
        assert enum(core_enums, name) == enum(kit_enums, name), f"kit {name} differs from core"


if __name__ == "__main__":
    main(*(Path(arg) for arg in sys.argv[1:]))
