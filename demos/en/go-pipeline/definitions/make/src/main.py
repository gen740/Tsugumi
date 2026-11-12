import outputs.out.t.Number as Number


def main(ctx):
    b = ctx.builder()
    Number.Start(b)
    Number.AddValue(b, 7)
    ctx.output("out", b, Number.End(b))
