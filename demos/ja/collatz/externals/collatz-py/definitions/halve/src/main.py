from inputs.number.collatz.Number import Number
import outputs.next.collatz.Number as next_out


def main(ctx):
    n = ctx.input("number", Number).Value()
    b = ctx.builder()
    next_out.Start(b)
    next_out.AddValue(b, n // 2)
    ctx.output("next", b, next_out.End(b))
