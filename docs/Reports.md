# R report Cards

> The demo described here is written for the previous Card ABI and is to be ported with the other demos ([TODO](Demos.md#todo-port-the-demos-to-card-abi-9)). This page describes the Cards as they are to be ported.

Open the **R reports** demo for working R Markdown and Quarto report Cards. Connect a typed upstream output to `data`, edit `src/report.Rmd` or `src/report.qmd`, and set the Card's parameters. The demo aggregates observations and renders both formats.

Each report Card is an ordinary R Card. Its `main.R` writes the input and parameters to a request file, renders the report with `Rscript render.R` in a fresh R process started through `processx`, and returns the rendered folder. `default.nix` fixes the renderer: it builds R with the report's packages, Pandoc and (for `.qmd`) Quarto from the project's pinned nixpkgs, and appends their store paths to `main.R`. Nothing is installed during a run. To use another package in the report, add it to the R used for rendering in `default.nix`.

## Inputs and parameters

Both formats declare the reserved `tsugumi` parameter in their YAML header:

```yaml
params:
  tsugumi: null
```

R chunks read the typed input messages through `params$tsugumi$inputs$data` and the Card parameters through `params$tsugumi$params`. A File or Directory input arrives as its message, with the path in `value`. Integers beyond R's exact double range are decimal strings, as in the R SDK; do not convert them to doubles if exact identity matters. The Card's ordinary settings control its inputs, parameters, deadline, network access and sandbox isolation.

## Rendering and saved results

`.Rmd` renders with `rmarkdown::render`; `.qmd` renders with Quarto's knitr engine. Quarto can also render `.Rmd`, but its project and format defaults can change an existing document's output, so the demo keeps `rmarkdown::render` for R Markdown.

Each call copies the report source into a scratch folder and renders there. Cache and freeze folders are removed, knitr chunk caching is disabled, and Quarto renders with `--no-cache` and execution enabled. A change to the source, the packages, the input or a parameter is therefore seen by the next call. A report must not read old results from absolute paths or keep its own cache.

The `report` Directory output holds the rendered HTML, the source, `tsugumi-params.json` with the parameters it rendered with, `metadata.json` naming the entry, and related files. The page shows its source in a collapsible section that works without JavaScript. The `html` File output names the rendered entry. Both outputs can feed downstream Cards. The demo retains the Directory as an artifact and records input and output values; Run History says which run produced it, and later edits and runs do not change it. While rendering, the Card shows a short note in its `markdown` view; the view is live and not saved.

HTML embeds figures, styles and other resources, including relative source assets. Open `index.html` in **Card settings → Artifacts** to view it, also after a restart. The Mac and Web previews are static: scripts and network requests are disabled. Download the HTML to inspect script-dependent widgets outside Tsugumi. The preview supports HTML up to 32 MiB; larger files remain exportable.

Renderer output in the Card's log keeps chunk names and source lines. A rendering error fails the call and returns no outputs. Cancelling, stopping the project or reaching the deadline ends the renderer's process tree; the next run starts fresh.

**PDF is not supported.** The renderer always selects HTML, even if the document declares another format. PDF would need an explicitly pinned TeX or Typst toolchain.

See [Quarto's R engine](https://quarto.org/docs/computations/r.html), [Quarto parameters](https://quarto.org/docs/computations/parameters.html) and [`rmarkdown::render`](https://pkgs.rstudio.com/rmarkdown/reference/render.html) for authoring details.
