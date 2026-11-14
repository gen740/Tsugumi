# Visualization Cards

Choose **Add Card → Visualization** and select Matplotlib, Plotly or Altair. The Python Card includes the data selection and plotting code in `src/main.py`, a `default.nix` environment and a `figure` Directory output. Set `kind` to change the figure. After editing the library templates under `ui/templates/visualization/`, run `python3 ui/templates/visualization.py` to regenerate the app bundles and demo source.

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
| `kind` | `line`, `scatter` or `histogram` |
| `data_path` | Dot-separated field path to the rows or columns; empty for the input root |
| `file` | Relative CSV/JSON member of a Directory input |
| `x`, `y` | Numeric column names; histogram uses only `x` |
| `series` | Optional column used to group series |
| `title`, `x_label`, `y_label` | Text labels, including Japanese |
| `width`, `height`, `dpi` | Figure size in inches and resolution; pixel size is inches × dpi |
| `theme` | `light` or `dark` |
| `max_points` | Maximum rows used by the figure, default 5,000; accepts 1–20,000 |
| `sample_every` | Use every n-th finite row in input order; defaults to 1 (all rows) |

Missing and non-finite coordinates are skipped. A Card fails with an explanation when the number of rows to plot exceeds `max_points`. Aggregate upstream, increase `max_points`, or explicitly sample with `sample_every`. The figure and `metadata.json` show how many finite rows were used and identify sampled histogram counts. Empty data produces an empty chart with a zero-row annotation. Missing axis columns produce a validation error.

The helper accepts at most 1,000,000 rows, 64 MiB per input file, and 100 plotted series. Width and height are 1–20 inches; dpi is 50–300. These bounds keep accidental inputs manageable; the existing interactive display JSON limit still applies, and long labels or wide rows can reach it before the point limit. For a larger dataset, reduce it in an upstream Card or edit the source to use an appropriate aggregation. The Card checks cancellation and deadlines while reading and filtering rows and before publishing the figure. Matplotlib figures close even when an export fails; the SDK's headless backend and per-invocation cleanup remain in use.

## Results, history and reports

Every successful output includes `plotted.csv` with the actual marks or histogram bins, and `metadata.json` with the counts, library, kind, run, Card and invocation. The invocation records its original inputs. The template enables `retainArtifact` with `artifactOutput: "figure"`, so the normal run/artifact UI keeps the Directory and its members. A downstream report Card can receive that same Directory and include its images or HTML. Pan, zoom and hover are temporary viewer state: they do not mutate Card parameters, recorded inputs, or exported figures. Change a parameter and run again to create a new result; old invocation displays and artifacts remain available after reopening the project.

The bilingual **Visualization → report** demo connects a typed aggregate to three Matplotlib figures and an HTML report. Change the aggregate's `offset` to update all figures without changing the data size. The report copies the saved figures and plotted values. Its HTML is deliberately a small Python example, independent of optional R Markdown or Quarto report tooling.

## Python helper

The scaffold calls `input_rows(inputs, run, field, member)` to read the connected input and `figure_result(run, folder, plotted, **facts)` to save the plotted values and return the Directory mapping. The Card's source owns column selection, filtering, sampling and drawing.

See `demos/en/visualization/definitions/line` for the complete Card definition. Plotly needs `ps.plotly` and Altair's offline export needs `ps.altair` and `ps.vl-convert-python`. The templates include `pkgs.ipaexfont` for Japanese labels.
