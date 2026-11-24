"""Mean of five deterministic measurements at each of 24 times."""
from math import sin

from card import Input, Output, params
from readings.Table import TableT


def run(input: Input) -> Output:
    xs = list(range(24))
    ys = [sum(10 + sin(t / 3) + j / 10 for j in range(5)) / 5 + params.offset for t in xs]
    # TODO(open): Port the demo display.
    return Output(data=TableT(x=xs, y=ys))
