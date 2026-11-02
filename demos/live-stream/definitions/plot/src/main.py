"""Shows the most recent `window` samples. State lives for one run (reuseWorker)."""
from collections import deque

from inputs.sample.live.Sample import Sample

points = None


def main(ctx):
    global points
    window = ctx.params().Window()
    s = ctx.input("sample", Sample)
    if points is None or s.Index() == 0:
        points = deque(maxlen=window)
    points.append((s.Index(), s.Value()))
    xs = [float(i) for i, _ in points]
    ys = [v for _, v in points]
    mean = sum(ys) / len(ys)
    ctx.display_line_chart([
        {"id": "value", "label": "random walk", "x": xs, "y": ys},
        {"id": "mean", "label": f"mean of last {len(ys)}", "x": [xs[0], xs[-1]], "y": [mean, mean]},
    ])
