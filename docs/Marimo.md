# marimo notebook Cards

Choose **Add Card → marimo notebook** on Mac or Web. The starting point is an ordinary Python Card containing editable `src/notebook.py`, its small `src/main.py` adapter, `default.nix`, and a typed output schema. The implementation uses the pinned `ps.marimo` package through the existing Python SDK. Add notebook dependencies in `default.nix`; no package installation happens when opening a result.

## One committed invocation

Connect a typed value to `data`. The default binding maps its `value` field to the notebook variable `source`, and the Card parameter `factor` to the notebook variable `factor`. The notebook computes `result`; the adapter emits that finite numeric value on `value` and the saved notebook Directory on `report`. Edit the output schema and packing code for other result types.

| Setting | Meaning |
| --- | --- |
| `notebook` | A `.py` notebook inside the Card's source directory, default `notebook.py` |
| `bindings` | JSON mapping notebook variables to `input.<port>.<field>` or `param.<field>` paths |
| `selected` | Notebook variable to emit as the starting point's numeric output |
| `factor` | Example ordinary Card parameter, available to the binding map |

The adapter reads generated input and parameter types. Bindings can pass scalar values, row/column structures, or a File/Directory's `value` path. The mapping must provide **all variables defined by an overridden configuration cell**: this is the behavior of [marimo's public `App.run(defs=…)` API](https://docs.marimo.io/api/app/). Put unrelated configuration in separate cells. Notebook source and the adapter remain editable in the normal Card source editor.

Each invocation copies its source into a private working directory and starts a fresh Python child process. After it succeeds, the worker copies the completed snapshot to the invocation's output directory. It calls `App.run()` once, obtains the returned cell outputs and definitions, and writes a static snapshot. The adapter publishes typed outputs only after that process succeeds. It does not call `marimo export` afterward: the [export command runs the notebook again](https://docs.marimo.io/guides/exporting/static_html/), which could repeat side effects. Notebook reactive dependencies determine the order of cells inside that one execution; they never directly emit Card messages or start downstream Cards.

The scaffold sets `reuseWorker: false`. Its child remains in the worker's process group. On failure, cancellation, deadline, project shutdown, and normal completion, the scheduler owns termination of that worker and anything it started. There is no detached session, browser, listening socket or interactive notebook server in this scaffold. Changing params and running again produces a new invocation and artifact, without rewriting a previous result.

## Saved results and source

The `report` Directory is retained as an artifact and includes:

- `index.html`: the static cell outputs, selected values and source-cell names/line numbers;
- `assets/`: saved PNG figures, also embedded as data images in HTML;
- `result.json`, `bindings.json`, `metadata.json`: selected definitions, supplied values and run/Card/invocation identity;
- `source/`: the exact notebook and adapter source copied for that invocation.

The static HTML includes ordinary Markdown/HTML, text and Matplotlib figures. It needs no JavaScript, CDN or running kernel. Frontend-only custom widgets are replaced by a note pointing to the saved source; this is a snapshot, not marimo's complete interactive frontend. The shared static HTML preview displays this self-contained HTML in both clients with scripts disabled. A saved result can be reopened from history after restarting the project. Editing the downloaded HTML does not change recorded results or send downstream messages.

A notebook error fails the Card and emits no typed result. Logs list source-cell names and source lines before execution, then retain the Python/marimo traceback. The failure diagnostic names the notebook; the source editor opens that saved or current `.py` file. Graph errors, missing bindings, absent selected variables and nonnumeric selected values are errors rather than partial successful reports.

In the bilingual **marimo notebook → typed result** demo, the upstream Card scales a measurement of 7 by `factor = 2`. The notebook visualizes the original and scaled values and sends the selected value of 14 to an ordinary Bash Card that logs and displays it. Change `factor` on the upstream Card to 3 and rerun it to get 21 and a new snapshot.
