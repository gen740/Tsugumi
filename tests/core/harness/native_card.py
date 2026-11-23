"""Build a tiny Card against the public C ABI for core RPC tests."""

import shutil
import subprocess

import flatbuffers
import pytest

from harness.daemon import ROOT
from Tsugumi.Card.CardPort import CardPortT
from Tsugumi.Card.CardResult import CardResultT


STATIC_RESULT_ENTRY = """
int tsugumi_main(const tsugumi_call* call, tsugumi_bytes* result) {
  if (call->abi_version != TSUGUMI_ABI_VERSION) return 1;
  *result = (tsugumi_bytes){result_bytes, sizeof(result_bytes)};
  return 0;
}
"""


def card_source(source, outputs):
    builder = flatbuffers.Builder(128)
    result = CardResultT(outputs=[CardPortT(name=name, bytes=value) for name, value in outputs])
    builder.Finish(result.Pack(builder))
    result_literal = "static const uint8_t result_bytes[] = {" + ",".join(map(str, builder.Output())) + "};\n"
    return "#include <tsugumi/card_abi.h>\n" + result_literal + source


def native_card(package, source, outputs=()):
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("a C compiler is required for the Card fixture")
    library = package / "lib"
    library.mkdir(parents=True)
    fixture = package / "card.c"
    fixture.write_text(card_source(source, outputs))
    subprocess.run([compiler, "-shared", "-fPIC", "-pthread", "-I",
                    str(ROOT / "protocol/include"), str(fixture), "-o",
                    str(library / "main.so")], check=True, capture_output=True, text=True, timeout=30)
    return str(package)
