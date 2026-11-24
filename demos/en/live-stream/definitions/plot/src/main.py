"""Draw a sliding window of samples in the resident plotting worker."""

from collections import deque

from card import Input, Output, params
from tsugumi import LineChart, LineSeries, send, view_open


points = deque(maxlen=params.window)
session = None


def run(inputs: Input) -> Output:
    global session
    sample = inputs.require("sample")
    if not points or sample.index <= points[-1][0]:
        points.clear()
        session = view_open("line-chart").session
    points.append((sample.index, sample.value))
    xs = [float(i) for i, _ in points]
    ys = [v for _, v in points]
    mean = sum(ys) / len(ys)
    send(session, LineChart([
        LineSeries("value", "random walk", xs, ys),
        LineSeries("mean", f"mean of last {len(ys)}", [xs[0], xs[-1]], [mean, mean]),
    ]))
    return Output()
