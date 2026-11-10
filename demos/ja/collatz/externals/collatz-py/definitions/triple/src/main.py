from inputs.number.collatz.Number import Number
import outputs.next.collatz.Number as next_out

from tsugumi import Failure

UINT64_MAX = (1 << 64) - 1


def main(ctx):
    n = ctx.input("number", Number).Value()
    result = 3 * n + 1
    if result > UINT64_MAX:
        raise Failure("output_invalid", f"integer overflow: 3*{n}+1 exceeds uint64")
    b = ctx.builder()
    next_out.Start(b)
    next_out.AddValue(b, result)
    ctx.output("next", b, next_out.End(b))
