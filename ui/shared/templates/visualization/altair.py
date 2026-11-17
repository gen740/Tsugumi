"""Draw the connected data with Altair. All of the drawing is in this file: edit it.

Params choose the columns and labels; kind is line, scatter or histogram.
select() chooses the points and main() draws them. The figure Directory
output keeps plot.json (Vega-Lite) and a self-contained plot.html, the values
that were drawn (plotted.csv) and metadata.json.
"""
import tsugumi
import math

import altair as alt
import numpy as np
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
    x_label = text(p.xLabel) or x
    y_label = text(p.yLabel) or ("count" if kind == "histogram" else y)
    color = {"color": alt.Color("series:N", title=series)} if series else {}
    dark = text(p.theme) == "dark"
    ink = "white" if dark else "#222"
    chart = alt.Chart(alt.Data(values=plotted)).properties(
        title=alt.TitleParams(
            text=text(p.title) or kind.title(),
            subtitle=facts["note"],
            subtitleColor="crimson" if facts["sampled"] else ink,
        ),
        width=round(p.width * p.dpi),
        height=round(p.height * p.dpi),
    )
    if kind == "histogram":
        chart = chart.mark_bar(opacity=0.7).encode(
            x=alt.X("left:Q", title=x_label), x2="right:Q", y=alt.Y("count:Q", title=y_label, stack=None), **color
        )
    else:
        chart = chart.mark_line() if kind == "line" else chart.mark_point()
        chart = chart.encode(
            x=alt.X("x:Q", title=x_label), y=alt.Y("y:Q", title=y_label), tooltip=["x:Q", "y:Q", "series:N"], **color
        ).interactive()
    chart = (
        chart.configure(font="TsugumiFigure", background="#222" if dark else "white")
        .configure_axis(labelColor=ink, titleColor=ink)
        .configure_legend(labelColor=ink, titleColor=ink)
        .configure_title(color=ink)
    )
    chart.save(folder / "plot.json")
    chart.save(folder / "plot.html", inline=True)
    embed_font(folder / "plot.html")
    report.figure(chart)

    yield figure_result(run, folder, plotted, library="altair", kind=kind, **facts)
    report.log(str(facts["note"]))
