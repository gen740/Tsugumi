# Visualization Cards

A Matplotlib Card can declare its environment in `default.nix` and save figures through an Artifact output. Set `kind` to change the figure. Each Card shows its figure live in its `image` view while it runs, and saves `plot.png`, `plot.svg` and `plot.pdf`; the saved files are what a later reader or a downstream Card uses. For an interactive chart, a Python Card can send a Plotly figure to the Python SDK's `plotly` view (`tsugumi.display.send(tsugumi.Plotly(fig))`, [Python SDK](https://github.com/gen740/tsugumi-kit/blob/main/docs/python-sdk.md)) and save its own HTML or JSON as an output. See the upstream [Matplotlib export API](https://matplotlib.org/stable/api/_as_gen/matplotlib.figure.Figure.savefig.html).

`default.nix` takes NumPy and Matplotlib from the project’s locked `<nixpkgs>` source. The Python SDK gives Matplotlib the kit's IPAexGothic as the Japanese fallback font, so Japanese labels render offline, the same in the live view and the saved files.

## Data and parameters

Connect an upstream output to `data`. The Card reads the connected message through the generated object API. A table can contain equally sized vector columns, or a vector of row tables selected with `data_path`. Scalar metadata beside column vectors is ignored. An Artifact input can carry UTF-8 CSV or JSON. The Card opens the directory and reads its `file` member, defaulting to `data.csv`. JSON supports the same row-table or column-vector shape. Directory members must stay inside the connected directory. Other formats or layouts can be decoded by editing the source.

| Parameter | Meaning |
| --- | --- |
| `kind` | `line`, `scatter` or `histogram` |
| `data_path` | Dot-separated field path to the rows or columns; empty for the input root |
| `file` | Relative CSV/JSON member of an Artifact directory |
| `x`, `y` | Numeric column names; histogram uses only `x` |
| `series` | Optional column used to group series |
| `title`, `x_label`, `y_label` | Text labels, including Japanese |
| `width`, `height`, `dpi` | Figure size in inches and resolution; pixel size is inches × dpi |
| `theme` | `light` or `dark` |
| `max_points` | Maximum rows used by the figure, default 5,000; accepts 1–20,000 |
| `sample_every` | Use every n-th finite row in input order; defaults to 1 (all rows) |

Missing and non-finite coordinates are skipped. A Card fails with an explanation when the number of rows to plot exceeds `max_points`. Aggregate upstream, increase `max_points`, or explicitly sample with `sample_every`. The figure and `metadata.json` say how many finite rows were used and mark sampled histogram counts. Empty data produces an empty chart with a note. Missing axis columns produce a validation error.

The Card accepts at most 1,000,000 rows, 64 MiB per input file and 100 plotted series. Width and height are 1–20 inches; dpi is 50–300. For a larger dataset, reduce it in an upstream Card or edit the source to aggregate. The Card checks cancellation and its deadline while reading and filtering rows and before drawing. Figures close even when an export fails.

## Results, history and reports

Every successful output includes `plotted.csv` with the actual marks or histogram bins and `metadata.json` with the counts, library and kind. The invocation records its inputs. The `figure` Artifact directory is sealed and available from Run History. The live view is not saved: to look at an earlier result, open its saved figure. Change a parameter and run again to create a new result next to the earlier ones.

A downstream Python Card can collect saved figures and their plotted values in a self-contained HTML report.
