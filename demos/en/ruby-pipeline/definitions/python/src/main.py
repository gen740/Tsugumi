import outputs.out.sample.Number as number

def main(ctx):
    value = ctx.params().Value()
    b = ctx.builder()
    number.Start(b)
    number.AddValue(b, value)
    ctx.output('out', b, number.End(b))
    ctx.display_scalar(value, 'Python')
