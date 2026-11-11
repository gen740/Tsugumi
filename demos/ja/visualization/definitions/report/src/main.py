"""Collect saved figure artifacts into a report that needs no network."""
from pathlib import Path
import shutil
from inputs.line.visualization.Files import Files as Line
from inputs.scatter.visualization.Files import Files as Scatter
from inputs.histogram.visualization.Files import Files as Histogram
import outputs.report.report.Files as output


def main(ctx):
    dest = Path(ctx.output_directory) / "report"
    dest.mkdir()
    sections = []
    for name, cls in [("line", Line), ("scatter", Scatter), ("histogram", Histogram)]:
        source = Path(ctx.input(name, cls).Value().decode())
        shutil.copytree(source, dest / name)
        sections.append(f'<section><h2>{name}</h2><img src="{name}/plot.png" alt="{name}"><p><a href="{name}/plot.svg">SVG</a> · <a href="{name}/plot.pdf">PDF</a> · <a href="{name}/source.csv">Source data</a></p></section>')
    (dest / "index.html").write_text('<!doctype html><meta charset="utf-8"><title>Visualization report</title><style>body{font:16px sans-serif;max-width:900px;margin:auto}img{max-width:100%}</style><h1>Visualization report</h1>' + ''.join(sections))
    b = ctx.builder(); path = b.CreateString(str(dest))
    output.Start(b); output.AddValue(b, path)
    ctx.output("report", b, output.End(b))
    ctx.display_text("Saved an offline report with PNG, SVG, PDF and source data for all three charts.")
