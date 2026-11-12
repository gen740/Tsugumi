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

# @select@


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
