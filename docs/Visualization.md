# Visualization Cards

Choose **Add Card → Visualization → library → Line, Scatter or Histogram** in the Mac or Web canvas. This creates an ordinary Python Card, with editable `src/main.py`, `default.nix`, parameters and a `figure` Directory output. It does not add another execution kind. The nine starting points share `ui/templates/visualization.py`; regenerate its JSON and Swift output after changing a starting point.

| Library | Card display | Saved figure files |
| --- | --- | --- |
| Matplotlib | PNG | `plot.png`, `plot.svg`, `plot.pdf` |
| Plotly | Interactive Plotly display | `plot.json`, self-contained `plot.html` |
| Altair | Interactive Vega-Lite display | `plot.json`, self-contained `plot.html` |

The format appears in the creation menu. Plotly and Altair templates do not promise static PNG, SVG or PDF exports; choose Matplotlib for those files. The HTML exports bundle their JavaScript and Japanese font, so opening a saved HTML file does not require a CDN. The normal interactive Card viewer also uses the app's bundled renderer. See the upstream [Matplotlib export API](https://matplotlib.org/stable/api/_as_gen/matplotlib.figure.Figure.savefig.html), [Plotly HTML export](https://plotly.com/python/interactive-html-export/) and [Altair saving guide](https://altair-viz.github.io/user_guide/saving_charts.html).

## Data and parameters

Connect an upstream output to `data`. The SDK reads the actual resolved FlatBuffers type using the generated object API. A table can contain equally sized vector columns, or a vector of row tables selected with `data_path`. Scalar metadata beside column vectors is ignored. A File input can contain UTF-8 CSV or JSON; a Directory input uses its `file` parameter, defaulting to `data.csv`. JSON supports the same row-table or column-vector shape. Directory members must stay inside the connected directory. Other formats or layouts can be decoded by editing the generated source.

| Parameter | Meaning |
| --- | --- |
| `data_path` | Dot-separated field path to the rows or columns; empty for the input root |
| `file` | Relative CSV/JSON member of a Directory input |
| `x`, `y` | Numeric column names; histogram uses only `x` |
| `series` | Optional column used to group series |
| `title`, `x_label`, `y_label` | Text labels, including Japanese |
| `width`, `height`, `dpi` | Figure size in inches and resolution; pixel size is inches × dpi |
| `theme` | `light` or `dark` |
| `max_points` | Maximum plotted rows, default 5,000; accepts 1–20,000 |

Missing and non-finite coordinates are skipped. If the finite rows exceed `max_points`, the scaffold takes evenly spaced rows in input order, including both endpoints. The figure visibly states how many rows were plotted and skipped; `metadata.json` records these counts. A histogram therefore counts the plotted sample, not the entire source. Aggregate upstream when that distinction matters. Empty data produces an empty chart with a zero-row annotation. A missing axis column fails with an explanation instead of silently drawing another column.

The helper accepts at most 1,000,000 rows, 64 MiB per input file, and 100 plotted series. Width and height are 1–20 inches; dpi is 50–300. These bounds keep accidental inputs manageable; the existing interactive display JSON limit still applies, and long labels or wide rows can reach it before the point limit. For a larger dataset, reduce it in an upstream Card or edit the source to use an appropriate aggregation. Cancellation and deadlines are checked while reading/filtering data and between exports. Matplotlib figures close even when an export fails; the SDK's headless backend and per-invocation cleanup remain in use.

## Results, history and reports

Every successful output includes `source.csv` (all decoded rows), `plotted.csv` (filtered/sampled coordinates), and `metadata.json` (counts, library, shape, run, Card and invocation). The template enables `retainArtifact` with `artifactOutput: "figure"`, so the normal run/artifact UI keeps the Directory and its members. A downstream report Card can receive that same Directory and include its images or HTML. Pan, zoom and hover are temporary viewer state: they do not mutate Card parameters, recorded source rows, or exported figures. Change a parameter and run again to create a new result; old invocation displays and artifacts remain available after reopening the project.

The bilingual **Visualization → report** demo connects a typed aggregate to three Matplotlib figures and an HTML report. Change the aggregate's `offset` to update all figures without changing the data size. The report copies the saved figures and source data; it does not take screenshots of the canvas. Its HTML is deliberately a small Python example, independent of optional R Markdown or Quarto report tooling.

## Python helper

The generated source is small and can be replaced entirely:

```python
from tsugumi.visualization import input_rows, render

def main(ctx):
    p = ctx.params()
    rows = input_rows(ctx, p.DataPath().decode(), p.File().decode())
    render(ctx, rows, library="matplotlib", kind="line")
```

Use the full scaffold in `demos/en/visualization/definitions/line` for the parameter declarations, Directory schema, pinned font and Nix dependencies. Plotly needs `ps.plotly`; Altair's offline export needs both `ps.altair` and `ps.vl-convert-python`. All templates include `pkgs.ipaexfont`. Figure source, parameters and output ownership are ordinary Card contracts, available to people and to the Agent through the same project tools.
