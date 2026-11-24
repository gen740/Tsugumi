"""Collect saved figure artifacts into a report that needs no network."""
from card import Artifact, Input, Output
import shutil


def run(inputs: Input) -> Output:
    artifact, dest = Artifact.dir()
    sections = []
    for name in ("line", "scatter", "histogram"):
        source = inputs.require(name).open()
        shutil.copytree(source, dest / name)
        sections.append(f'<section><h2>{name}</h2><img src="{name}/plot.png" alt="{name}"><p><a href="{name}/plot.svg">SVG</a> · <a href="{name}/plot.pdf">PDF</a> · <a href="{name}/plotted.csv">Plotted data</a></p></section>')
    (dest / "index.html").write_text('<!doctype html><meta charset="utf-8"><title>Visualization report</title><style>body{font:16px sans-serif;max-width:900px;margin:auto}img{max-width:100%}</style><h1>Visualization report</h1>' + ''.join(sections))
    # TODO(open): Port the demo display.
    return Output(report=artifact)
