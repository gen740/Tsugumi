# R report Cards

Use **Add Card → Reports (HTML; PDF unsupported)** to create an R Markdown or Quarto report. Connect a typed upstream output to `data`, edit `src/report.Rmd` or `src/report.qmd`, and set the Card's parameters. The bundled **R reports** demo aggregates observations and renders both formats.

## Inputs and parameters

Both formats declare the reserved `tsugumi` parameter in their YAML header:

```yaml
params:
  tsugumi: null
```

R chunks read typed input messages through `params$tsugumi$inputs$data`, Card parameters through `params$tsugumi$params`, and File/Directory input paths through `params$tsugumi$paths$data`. Environment port names become lower-case keys, matching the generated templates. Integers beyond R's exact double range are strings, as in the R JSON adapter. Do not convert them to doubles if exact identity matters.

The ordinary Card settings control inputs, params, execution deadlines, network and filesystem access. `default.nix` selects the entry and dependencies:

```nix
{ pkgs, inputs, params }: {
  run = inputs.tsugumi.reportCard {
    src = ./src;
    entry = "report.qmd";
    packages = ps: [ ps.ggplot2 ];
  };
}
```

The project's pinned Nixpkgs fixes R, knitr, rmarkdown, Pandoc, Quarto and extra R packages. No package installation happens implicitly during a Run. Font packages are configurable through `fonts`, with Latin and Japanese defaults.

## Rendering and saved results

`.Rmd` uses `rmarkdown::render`; `.qmd` uses Quarto with the knitr engine. Quarto can also render `.Rmd`, but its project and format defaults can change an existing document's output. Using `rmarkdown::render` directly preserves the R Markdown render path and its smaller dependency set. Quarto's renderer supports `.qmd` options such as its `format` configuration and Quarto project settings. Both run as CLI adapters using the existing bash/JSON Card contract. They share R's data conventions, but do not require or modify the resident R SDK.

Each invocation starts a fresh R process and copies the source into its own temporary directory. Standard cache/freeze folders are excluded; R Markdown cache hooks and Quarto's `--no-cache` disable chunk cache reuse. Quarto renders the single entry with execution enabled, rather than reusing a frozen project. Its output directory is overridden inside the invocation; nested documents render from their own directory so relative assets still resolve. Source, dependency, input or parameter edits are therefore observed by the next invocation. Reports must not deliberately read old results from absolute paths or implement their own persistent cache.

The `report` Directory output contains the rendered HTML, source, parameter provenance and related files. The rendered page shows its source in a collapsible section that works without JavaScript. `metadata.json` identifies the source entry, run, Card and invocation. The `html` File output names the rendered entry. The templates retain the Directory as an artifact and record input/output values. Both outputs can feed downstream Cards. Saving an artifact copies it into history, so later edits and Runs do not change earlier reports.

HTML embeds figures, styles and other resources, including relative source assets. Open `index.html` in **Card settings → Artifacts** to view it after execution or restart. The Mac and Web previews are static: scripts and network requests are disabled. Download the HTML when inspecting script-dependent widgets outside Tsugumi. The preview supports HTML up to 32 MiB; larger files remain exportable.

Renderer stderr retains chunk names and source line diagnostics. Warnings appear in the generated document and renderer output according to the document's knitr options. Rendering errors fail the invocation and do not send outputs. Cancelling, stopping the project or reaching the execution deadline ends the worker's process group, including Quarto/R children; a subsequent Run starts fresh. The Linux SDK and system checks exercise both renderers through the daemon, including cancellation and a Quarto project with stale freeze files.

**PDF is not supported by this adapter.** It always selects HTML, even if the document declares another output format. PDF requires an explicitly pinned TeX or Typst toolchain and separate rendering/export validation; it is not installed on demand.

See [Quarto's R engine](https://quarto.org/docs/computations/r.html), [Quarto parameters](https://quarto.org/docs/computations/parameters.html) and [`rmarkdown::render`](https://pkgs.rstudio.com/rmarkdown/reference/render.html) for authoring details.
