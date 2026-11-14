"""Mean of five deterministic measurements at each of 24 times."""
import math


def main(inputs, report):
    outputs = {}
    xs = list(range(24))
    ys = [sum(10 + math.sin(t / 3) + j / 10 for j in range(5)) / 5 + inputs.params.offset for t in xs]
    outputs.update({'data': {
        'x': xs,
        'y': ys,
    }})
    report.table(["time", "mean"], [[x, y] for x, y in zip(xs, ys)])
    return outputs
