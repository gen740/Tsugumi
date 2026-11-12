"""Mean of five deterministic measurements at each of 24 times."""
import math
import outputs.data.readings.Table as output


def main(ctx):
    xs = list(range(24))
    ys = [sum(10 + math.sin(t / 3) + j / 10 for j in range(5)) / 5 + ctx.params().Offset() for t in xs]
    b = ctx.builder()
    output.StartXVector(b, len(xs))
    for x in reversed(xs): b.PrependFloat64(x)
    x = b.EndVector()
    output.StartYVector(b, len(ys))
    for y in reversed(ys): b.PrependFloat64(y)
    y = b.EndVector()
    output.Start(b); output.AddX(b, x); output.AddY(b, y)
    ctx.output("data", b, output.End(b))
    ctx.display_table(["time", "mean"], [[x, y] for x, y in zip(xs, ys)])
