"""Build a tiny Card against the public C ABI for core RPC tests."""

import shutil
import subprocess

import flatbuffers
import pytest

from harness.daemon import ROOT
from Tsugumi.Card.CardPort import CardPortT
from Tsugumi.Card.CardResult import CardResultT


def native_card(package, source, outputs=()):
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("a C compiler is required for the Card fixture")
    builder = flatbuffers.Builder(128)
    result = CardResultT(outputs=[CardPortT(name=name, kind=kind, bytes=value) for name, kind, value in outputs])
    builder.Finish(result.Pack(builder))
    result_literal = "static const uint8_t result_bytes[] = {" + ",".join(map(str, builder.Output())) + "};\n"
    library = package / "lib"
    library.mkdir(parents=True)
    fixture = package / "card.c"
    fixture.write_text("#include <tsugumi/card_abi.h>\n" + result_literal + source)
    subprocess.run([compiler, "-shared", "-fPIC", "-pthread", "-I",
                    str(ROOT / "protocol/include"), str(fixture), "-o",
                    str(library / "main.so")], check=True, capture_output=True, text=True, timeout=30)
    return str(package)
