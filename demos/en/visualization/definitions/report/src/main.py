"""Collect saved figure artifacts into a report that needs no network."""
from pathlib import Path
import shutil


def main(inputs, report, run):
    outputs = {}
    dest = Path(run.output_directory) / "report"
    dest.mkdir()
    sections = []
    for name in ("line", "scatter", "histogram"):
        source = Path(getattr(inputs, name).value.decode())
        shutil.copytree(source, dest / name)
        sections.append(f'<section><h2>{name}</h2><img src="{name}/plot.png" alt="{name}"><p><a href="{name}/plot.svg">SVG</a> · <a href="{name}/plot.pdf">PDF</a> · <a href="{name}/plotted.csv">Plotted data</a></p></section>')
    (dest / "index.html").write_text('<!doctype html><meta charset="utf-8"><title>Visualization report</title><style>body{font:16px sans-serif;max-width:900px;margin:auto}img{max-width:100%}</style><h1>Visualization report</h1>' + ''.join(sections))
    path = str(dest)
    outputs.update({'report': {'value': path}})
    report.text("Saved an offline report with PNG, SVG, PDF and the plotted data for all three charts.")
    return outputs
