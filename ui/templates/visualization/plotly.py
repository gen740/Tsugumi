"""Draw the connected data with Plotly. All of the drawing is in this file: edit it.

Params choose the columns and labels; kind is line, scatter or histogram.
select() chooses the points and main() draws them. The figure Directory
output keeps plot.json and a self-contained plot.html (it carries plotly.js,
about 5 MB), the values that were drawn (plotted.csv) and metadata.json.
"""
import tsugumi
import math

import numpy as np
import plotly.graph_objects as go
from tsugumi import Failure
from tsugumi.visualization import embed_font, figure_result, figure_directory, input_rows

# @select@


def main(inputs, report, run):
    p = inputs.params
    kind, x, y, series = text(p.kind), text(p.x), text(p.y), text(p.series)
    rows = input_rows(inputs, run, field=text(p.dataPath), member=text(p.file))
    points, facts = select(inputs, run, rows, kind, x, y, series)
    groups, plotted = marks(points, kind)

    folder = figure_directory(run)
    fig = go.Figure()
    for name, group in groups.items():
        if kind == "histogram":
            centers = [(a + b) / 2 for a, b in zip(group["left"], group["right"])]
            widths = [b - a for a, b in zip(group["left"], group["right"])]
            fig.add_trace(go.Bar(x=centers, y=group["count"], width=widths, name=name, opacity=0.7))
        else:
            fig.add_trace(go.Scatter(x=group["x"], y=group["y"], name=name, mode="lines" if kind == "line" else "markers"))
    fig.update_layout(
        title=text(p.title) or kind.title(),
        xaxis_title=text(p.xLabel) or x,
        yaxis_title=text(p.yLabel) or ("count" if kind == "histogram" else y),
        template="plotly_dark" if text(p.theme) == "dark" else "plotly_white",
        font_family="TsugumiFigure, sans-serif",
        showlegend=bool(series),
        barmode="overlay",
        width=round(p.width * p.dpi),
        height=round(p.height * p.dpi),
    )
    fig.add_annotation(
        text=facts["note"], x=0, y=-0.22, xref="paper", yref="paper", showarrow=False,
        font_color="crimson" if facts["sampled"] else None,
    )
    if not points:
        fig.add_annotation(text="No finite data", x=0.5, y=0.5, xref="paper", yref="paper", showarrow=False)
    fig.write_json(folder / "plot.json")
    fig.write_html(folder / "plot.html", include_plotlyjs=True, full_html=True)
    embed_font(folder / "plot.html")
    report.figure(fig)

    yield figure_result(run, folder, plotted, library="plotly", kind=kind, **facts)
    report.log(str(facts["note"]))
