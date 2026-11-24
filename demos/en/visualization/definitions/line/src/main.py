"""Draw the connected data with Matplotlib. All of the drawing is in this file: edit it.

Params choose the columns and labels; kind is line, scatter or histogram.
select() chooses the points and main() draws them. The figure Directory
output keeps plot.png, plot.svg and plot.pdf, the values that were drawn
(plotted.csv) and metadata.json.
"""
import csv
import json

import tsugumi
from card import Artifact, Input, Output, params
import math

import matplotlib.pyplot as plt
import numpy as np
from tsugumi import Failure

def text(value):
    return value.decode() if isinstance(value, bytes) else (value or "")


def select(rows, kind, x, y, series):
    """The points to draw, as (x, y, series), and what was done to choose them.

    Rows whose coordinates are not finite numbers are left out and counted.
    When more than max_points remain, aggregate upstream, raise max_points,
    or explicitly set sample_every to use every n-th finite row. Histograms
    built from a sample describe that sample's bin counts."""
    p = params
    if kind not in ("line", "scatter", "histogram"):
        raise Failure("validation_failed", "kind must be line, scatter or histogram")
    if not 1 <= p.max_points <= 20_000:
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
            if tsugumi.cancelled():
                raise Failure("cancelled", "Visualization cancelled")
            if tsugumi.remaining_ms() == 0:
                raise Failure("timeout", "Visualization deadline reached")
        try:
            values = [float(row[axis]) for axis in axes]
        except (KeyError, TypeError, ValueError):
            continue
        if all(math.isfinite(v) for v in values):
            points.append((values[0], values[-1], str(row.get(series, "")) if series else ""))
    finite = len(points)
    note = f"{finite} of {len(rows)} rows have finite values"

    every = p.sample_every
    if every < 1:
        raise Failure("validation_failed", "sample_every must be at least 1")
    if every > 1:
        points = points[::every]
        note += f"; SAMPLED: 1 point in {every} drawn ({len(points)} of {finite})"
        if kind == "histogram":
            note += "; bin counts describe the sample"
    if len(points) > p.max_points:
        raise Failure(
            "validation_failed",
            f"{len(points)} points to draw exceed max_points ({p.max_points}): "
            "aggregate upstream, raise max_points, or set sample_every to draw every n-th point",
        )
    if len({point[2] for point in points}) > 100:
        raise Failure("validation_failed", "More than 100 series; aggregate or choose another series column")
    facts = dict(
        source_rows=len(rows),
        finite_rows=finite,
        drawn_rows=len(points),
        max_points=p.max_points,
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


def run(inputs: Input) -> Output:
    p = params
    kind, x, y, series = text(p.kind), text(p.x), text(p.y), text(p.series)
    table = inputs.require("data")
    if isinstance(table, Artifact):
        directory = table.open()
        file = directory / p.file
        if not file.resolve().is_relative_to(directory.resolve()):
            raise Failure("input_invalid", "the selected file is outside the artifact")
        with file.open(encoding="utf-8", newline="") as stream:
            data = json.load(stream) if file.suffix == ".json" else list(csv.DictReader(stream))
    else:
        data = vars(table)
    for key in p.data_path.split(".") if p.data_path else []:
        data = data[key] if isinstance(data, dict) else getattr(data, key)
    if isinstance(data, dict):
        columns = {key: value for key, value in data.items() if isinstance(value, (list, tuple, np.ndarray))}
        rows = [dict(zip(columns, values)) for values in zip(*columns.values(), strict=True)]
    else:
        rows = [row if isinstance(row, dict) else vars(row) for row in data]
    if len(rows) > 1_000_000:
        raise Failure("input_invalid", "more than 1,000,000 rows")
    points, facts = select(rows, kind, x, y, series)
    groups, plotted = marks(points, kind)

    artifact, folder = Artifact.dir()
    with plt.style.context(["dark_background"] if text(p.theme) == "dark" else []):
        fig, ax = plt.subplots(figsize=(p.width, p.height), dpi=p.dpi)
        try:
            for name, group in groups.items():
                if kind == "histogram":
                    ax.stairs(group["count"], group["left"] + group["right"][-1:], fill=True, alpha=0.7, label=name)
                elif kind == "line":
                    ax.plot(group["x"], group["y"], label=name)
                else:
                    ax.scatter(group["x"], group["y"], label=name)
            ax.set(
                title=text(p.title) or kind.title(),
                xlabel=text(p.x_label) or x,
                ylabel=text(p.y_label) or ("count" if kind == "histogram" else y),
            )
            if series and groups:
                ax.legend()
            if not points:
                ax.text(0.5, 0.5, "No finite data", transform=ax.transAxes, ha="center")
            fig.text(0.02, 0.01, facts["note"], fontsize=8, color="tab:red" if facts["sampled"] else None)
            fig.tight_layout(rect=(0, 0.05, 1, 1))
            for extension in ("png", "svg", "pdf"):
                fig.savefig(folder / f"plot.{extension}")
            # TODO(open): Port the demo display.
        finally:
            plt.close(fig)

    with (folder / "plotted.csv").open("w", encoding="utf-8", newline="") as stream:
        columns = ["left", "right", "count", "series"] if kind == "histogram" else ["x", "y", "series"]
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(plotted)
    (folder / "metadata.json").write_text(
        json.dumps(dict(library="matplotlib", kind=kind, **facts), ensure_ascii=False), encoding="utf-8"
    )
    print(facts["note"])
    return Output(figure=artifact)
