from inputs.cpp.collatz.Sequence import Sequence as CppSequence
from inputs.python.collatz.Sequence import Sequence as PySequence
import outputs.sequence.collatz.Sequence as seq_out
import outputs.verdict.collatz.Verdict as verdict_out

from tsugumi import Failure


def values(seq):
    return [seq.Values(i) for i in range(seq.ValuesLength())]


def main(ctx):
    a = values(ctx.input("cpp", CppSequence))
    b = values(ctx.input("python", PySequence))
    if a != b:
        raise Failure("output_invalid", f"sequences differ: C++ {len(a)} values, Python {len(b)} values")
    ctx.display_markdown(
        f"**C++ and Python agree**: {len(a)} values, $a_0 = {a[0]}$ reaches $1$ "
        f"after ${len(a) - 1}$ steps (peak $\\max_k a_k = {max(a)}$)."
    )
    builder = ctx.builder()
    seq_out.StartValuesVector(builder, len(a))
    for v in reversed(a):
        builder.PrependUint64(v)
    vector = builder.EndVector()
    seq_out.Start(builder)
    seq_out.AddValues(builder, vector)
    sequence = seq_out.End(builder)
    verdict_out.Start(builder)
    verdict_out.AddEqual(builder, True)
    verdict_out.AddCount(builder, len(a))
    verdict = verdict_out.End(builder)
    ctx.output("verdict", builder, verdict)
    ctx.output("sequence", builder, sequence)
