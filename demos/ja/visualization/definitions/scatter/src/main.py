"""Draw the connected data with Matplotlib. All of the drawing is in this file: edit it.

Params choose the columns and labels; kind is line, scatter or histogram.
select() chooses the points and main() draws them. The figure Directory
output keeps plot.png, plot.svg and plot.pdf, the values that were drawn
(plotted.csv) and metadata.json.
"""
import math

import matplotlib.pyplot as plt
import numpy as np
from tsugumi import Failure
from tsugumi.visualization import emit_figure, figure_directory, input_rows

def text(value):
    return value.decode() if value else ""


def select(ctx, rows, kind, x, y, series):
    """The points to draw, as (x, y, series), and what was done to choose them.

    Rows whose coordinates are not finite numbers are left out and counted.
    When more than max_points remain, aggregate upstream, raise max_points,
    or explicitly set sample_every to use every n-th finite row. Histograms
    built from a sample describe that sample's bin counts."""
    p = ctx.params()
    if kind not in ("line", "scatter", "histogram"):
        raise Failure("validation_failed", "kind must be line, scatter or histogram")
    if not 1 <= p.MaxPoints() <= 20_000:
        raise Failure("validation_failed", "max_points must be between 1 and 20,000")
    if not (1 <= p.Width() <= 20 and 1 <= p.Height() <= 20 and 50 <= p.Dpi() <= 300):
        raise Failure("validation_failed", "width and height must be 1–20 inches and dpi 50–300")
    if text(p.Theme()) not in ("light", "dark"):
        raise Failure("validation_failed", "theme must be light or dark")
    axes = [x] if kind == "histogram" else [x, y]
    for axis in axes:
        if rows and not any(axis in row for row in rows):
            raise Failure("validation_failed", f"Column {axis!r} is not in the input; set x and y to its columns")
    if series and rows and not any(series in row for row in rows):
        raise Failure("validation_failed", f"Series column {series!r} is not in the input")
    points = []
    for i, row in enumerate(rows):
        if i % 4096 == 0:
            if ctx.cancelled():
                raise Failure("cancelled", "Visualization cancelled")
            if ctx.remaining_ms() == 0:
                raise Failure("timeout", "Visualization deadline reached")
        try:
            values = [float(row[axis]) for axis in axes]
        except (KeyError, TypeError, ValueError):
            continue
        if all(math.isfinite(v) for v in values):
            points.append((values[0], values[-1], str(row.get(series, "")) if series else ""))
    finite = len(points)
    note = f"{finite} of {len(rows)} rows have finite values"

    every = p.SampleEvery()
    if every < 1:
        raise Failure("validation_failed", "sample_every must be at least 1")
    if every > 1:
        points = points[::every]
        note += f"; SAMPLED: 1 point in {every} drawn ({len(points)} of {finite})"
        if kind == "histogram":
            note += "; bin counts describe the sample"
    if len(points) > p.MaxPoints():
        raise Failure(
            "validation_failed",
            f"{len(points)} points to draw exceed max_points ({p.MaxPoints()}): "
            "aggregate upstream, raise max_points, or set sample_every to draw every n-th point",
        )
    if len({point[2] for point in points}) > 100:
        raise Failure("validation_failed", "More than 100 series; aggregate or choose another series column")
    facts = dict(
        source_rows=len(rows),
        finite_rows=finite,
        drawn_rows=len(points),
        max_points=p.MaxPoints(),
        sample_every=every,
        sampled=every > 1,
        note=note,
    )
    return points, facts


def marks(points, kind):
    """The marks of each series, and the same values as rows for plotted.csv.
    A line or scatter series has x and y; a histogram series has one count
    per bin (left, right), with the same bins for every series."""
    names = list(dict.fromkeys(q[2] for q in points))
    if kind == "histogram":
        bins = min(100, max(1, math.ceil(math.sqrt(len(points)))))
        edges = np.histogram_bin_edges([q[0] for q in points], bins=bins).tolist()
        groups = {
            name: {
                "left": edges[:-1],
                "right": edges[1:],
                "count": np.histogram([q[0] for q in points if q[2] == name], bins=edges)[0].tolist(),
            }
            for name in names
        }
    else:
        groups = {
            name: {"x": [q[0] for q in points if q[2] == name], "y": [q[1] for q in points if q[2] == name]}
            for name in names
        }
    plotted = [
        {**{key: values[i] for key, values in group.items()}, "series": name}
        for name, group in groups.items()
        for i in range(len(next(iter(group.values()))))
    ]
    return groups, plotted


def main(ctx):
    p = ctx.params()
    kind, x, y, series = text(p.Kind()), text(p.X()), text(p.Y()), text(p.Series())
    rows = input_rows(ctx, field=text(p.DataPath()), member=text(p.File()))
    points, facts = select(ctx, rows, kind, x, y, series)
    groups, plotted = marks(points, kind)

    folder = figure_directory(ctx)
    with plt.style.context(["dark_background"] if text(p.Theme()) == "dark" else []):
        fig, ax = plt.subplots(figsize=(p.Width(), p.Height()), dpi=p.Dpi())
        try:
            for name, group in groups.items():
                if kind == "histogram":
                    ax.stairs(group["count"], group["left"] + group["right"][-1:], fill=True, alpha=0.7, label=name)
                elif kind == "line":
                    ax.plot(group["x"], group["y"], label=name)
                else:
                    ax.scatter(group["x"], group["y"], label=name)
            ax.set(
                title=text(p.Title()) or kind.title(),
                xlabel=text(p.XLabel()) or x,
                ylabel=text(p.YLabel()) or ("count" if kind == "histogram" else y),
            )
            if series and groups:
                ax.legend()
            if not points:
                ax.text(0.5, 0.5, "No finite data", transform=ax.transAxes, ha="center")
            fig.text(0.02, 0.01, facts["note"], fontsize=8, color="tab:red" if facts["sampled"] else None)
            fig.tight_layout(rect=(0, 0.05, 1, 1))
            for extension in ("png", "svg", "pdf"):
                fig.savefig(folder / f"plot.{extension}")
            ctx.display_figure(fig)
        finally:
            plt.close(fig)

    emit_figure(ctx, folder, plotted, library="matplotlib", kind=kind, **facts)
    print(facts["note"])
