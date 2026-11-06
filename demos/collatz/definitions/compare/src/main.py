from inputs.cpp.collatz.Sequence import Sequence as CppSequence
from inputs.julia.collatz.Sequence import Sequence as JuliaSequence
from inputs.python.collatz.Sequence import Sequence as PySequence
from inputs.r.collatz.Sequence import Sequence as RSequence
import outputs.sequence.collatz.Sequence as seq_out
import outputs.verdict.collatz.Verdict as verdict_out

from tsugumi import Failure


def values(seq):
    return [seq.Values(i) for i in range(seq.ValuesLength())]


def main(ctx):
    found = {
        "C++": values(ctx.input("cpp", CppSequence)),
        "Python": values(ctx.input("python", PySequence)),
        "Julia": values(ctx.input("julia", JuliaSequence)),
        "R": values(ctx.input("r", RSequence)),
    }
    a = found["C++"]
    if any(v != a for v in found.values()):
        raise Failure("output_invalid", "sequences differ: " + ", ".join(f"{n} {len(v)} values" for n, v in found.items()))
    ctx.display_markdown(
        f"**C++, Python, Julia and R agree**: {len(a)} values, $a_0 = {a[0]}$ reaches $1$ "
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
