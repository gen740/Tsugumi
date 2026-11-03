"""Shows the most recent `window` samples. The worker stays across runs
(reuseWorker); each run starts with an empty window."""
from collections import deque

from inputs.sample.live.Sample import Sample

points = None


def on_run_start(ctx):
    global points
    points = deque(maxlen=ctx.params().Window())


def main(ctx):
    s = ctx.input("sample", Sample)
    points.append((s.Index(), s.Value()))
    xs = [float(i) for i, _ in points]
    ys = [v for _, v in points]
    mean = sum(ys) / len(ys)
    ctx.display_line_chart([
        {"id": "value", "label": "random walk", "x": xs, "y": ys},
        {"id": "mean", "label": f"mean of last {len(ys)}", "x": [xs[0], xs[-1]], "y": [mean, mean]},
    ])
