"""Connect data, set column names in params, then run.

Matplotlib saves PNG/SVG/PDF; Plotly/Altair save offline HTML and JSON.
The figure Directory output also contains source.csv, plotted.csv and metadata.
Edit this ordinary Python source when the chart needs more than its params.
"""
from tsugumi.visualization import input_rows, render


def main(ctx):
    p = ctx.params()
    rows = input_rows(ctx, field=p.DataPath().decode(), member=p.File().decode())
    render(ctx, rows, library='matplotlib', kind='scatter')
