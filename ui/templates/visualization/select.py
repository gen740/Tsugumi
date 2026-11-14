import tsugumi
# How every Visualization template chooses the points it draws. The generator
# (ui/templates/visualization.py) writes what follows the marker below into
# each library's src/main.py in place of its "# @select@" line, so a Card
# carries all of its selection code; the imports above the marker are in
# each library's source already.
import math

import numpy as np
from tsugumi import Failure

# @insert@


def text(value):
    return value.decode() if value else ""


def select(inputs, run, rows, kind, x, y, series):
    """The points to draw, as (x, y, series), and what was done to choose them.

    Rows whose coordinates are not finite numbers are left out and counted.
    When more than max_points remain, aggregate upstream, raise max_points,
    or explicitly set sample_every to use every n-th finite row. Histograms
    built from a sample describe that sample's bin counts."""
    p = inputs.params
    if kind not in ("line", "scatter", "histogram"):
        raise Failure("validation_failed", "kind must be line, scatter or histogram")
    if not 1 <= p.maxPoints <= 20_000:
        raise Failure("validation_failed", "max_points must be between 1 and 20,000")
    if not (1 <= p.width <= 20 and 1 <= p.height <= 20 and 50 <= p.dpi <= 300):
        raise Failure("validation_failed", "width and height must be 1–20 inches and dpi 50–300")
    if text(p.theme) not in ("light", "dark"):
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
            if run.cancelled():
                raise Failure("cancelled", "Visualization cancelled")
            if run.remaining_ms() == 0:
                raise Failure("timeout", "Visualization deadline reached")
        try:
            values = [float(row[axis]) for axis in axes]
        except (KeyError, TypeError, ValueError):
            continue
        if all(math.isfinite(v) for v in values):
            points.append((values[0], values[-1], str(row.get(series, "")) if series else ""))
    finite = len(points)
    note = f"{finite} of {len(rows)} rows have finite values"

    every = p.sampleEvery
    if every < 1:
        raise Failure("validation_failed", "sample_every must be at least 1")
    if every > 1:
        points = points[::every]
        note += f"; SAMPLED: 1 point in {every} drawn ({len(points)} of {finite})"
        if kind == "histogram":
            note += "; bin counts describe the sample"
    if len(points) > p.maxPoints:
        raise Failure(
            "validation_failed",
            f"{len(points)} points to draw exceed max_points ({p.maxPoints}): "
            "aggregate upstream, raise max_points, or set sample_every to draw every n-th point",
        )
    if len({point[2] for point in points}) > 100:
        raise Failure("validation_failed", "More than 100 series; aggregate or choose another series column")
    facts = dict(
        source_rows=len(rows),
        finite_rows=finite,
        drawn_rows=len(points),
        max_points=p.maxPoints,
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
