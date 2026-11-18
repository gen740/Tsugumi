"""Draw the connected data with Matplotlib. All of the drawing is in this file: edit it.

Params choose the columns and labels; kind is line, scatter or histogram.
select() chooses the points and main() draws them. The figure Directory
output keeps plot.png, plot.svg and plot.pdf, the values that were drawn
(plotted.csv) and metadata.json.
"""
import tsugumi
import math

import matplotlib.pyplot as plt
import numpy as np
from tsugumi import Failure
from tsugumi.visualization import figure_result, figure_directory, input_rows

# @select@


def main(inputs, report, run):
    p = inputs.params
    kind, x, y, series = text(p.kind), text(p.x), text(p.y), text(p.series)
    rows = input_rows(inputs, run, field=text(p.dataPath), member=text(p.file))
    points, facts = select(inputs, run, rows, kind, x, y, series)
    groups, plotted = marks(points, kind)

    folder = figure_directory(run)
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
                xlabel=text(p.xLabel) or x,
                ylabel=text(p.yLabel) or ("count" if kind == "histogram" else y),
            )
            if series and groups:
                ax.legend()
            if not points:
                ax.text(0.5, 0.5, "No finite data", transform=ax.transAxes, ha="center")
            fig.text(0.02, 0.01, facts["note"], fontsize=8, color="tab:red" if facts["sampled"] else None)
            fig.tight_layout(rect=(0, 0.05, 1, 1))
            for extension in ("png", "svg", "pdf"):
                fig.savefig(folder / f"plot.{extension}")
            report.figure(fig)
        finally:
            plt.close(fig)

    yield figure_result(run, folder, plotted, library="matplotlib", kind=kind, **facts)
    report.log(str(facts["note"]))
