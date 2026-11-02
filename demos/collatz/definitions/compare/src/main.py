from inputs.cpp.collatz.Sequence import Sequence as CppSequence
from inputs.python.collatz.Sequence import Sequence as PySequence
import outputs.verdict.collatz.Verdict as verdict_out

from tsugumi import Failure


def values(seq):
    return [seq.Values(i) for i in range(seq.ValuesLength())]


def main(ctx):
    a = values(ctx.input("cpp", CppSequence))
    b = values(ctx.input("python", PySequence))
    if a != b:
        raise Failure("output_invalid", f"sequences differ: C++ {len(a)} values, Python {len(b)} values")
    ctx.display_text(f"C++ and Python agree: {len(a)} values, {len(a) - 1} steps to 1")
    builder = ctx.builder()
    verdict_out.Start(builder)
    verdict_out.AddEqual(builder, True)
    verdict_out.AddCount(builder, len(a))
    ctx.output("verdict", builder, verdict_out.End(builder))
