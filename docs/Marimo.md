# marimo notebook Cards

> This page describes the planned notebook Card behavior. See [demo status](Demos.md#todo-port-the-demos-to-the-current-card-abi).

Open the **marimo notebook → typed result** demo for a complete notebook Card. It is an ordinary Python Card: `src/notebook.py` is the editable notebook, `src/main.py` maps the Card's input to notebook variables and returns the result, and `src/runner.py` runs the notebook in a child process. `default.nix` takes marimo and Matplotlib from the project’s locked `<nixpkgs>` source (`packages = ps: [ ps.marimo ps.matplotlib ];`) and the Python SDK supplies the kit's IPAexGothic as Matplotlib's Japanese fallback font. Nothing is installed when a notebook runs or when a result is opened.

## One committed invocation

Connect a typed value to `data`. The Card's parameters say what goes in and what comes out:

| Parameter | Meaning |
| --- | --- |
| `notebook` | A `.py` notebook inside the Card's source directory, default `notebook.py` |
| `bindings` | JSON mapping notebook variables to `inputs.<port>.<field>` or `param.<field>` paths |
| `selected` | The notebook variable returned on the `value` output |
| `factor` | An ordinary parameter of the example, available to the bindings |

The default bindings map `inputs.data.source` to the notebook variable `source` and `param.factor` to `factor`. The notebook computes `result`, and the Card returns it on `value` with the saved notebook folder on the `report` Directory output. To return another type, edit `_outputs.fbs` and the end of `main.py`.

A binding replaces a variable the notebook defines, so the mapping must provide **all variables defined by an overridden cell**: this is the behaviour of [marimo's `App.run(defs=…)`](https://docs.marimo.io/api/app/). Put unrelated configuration in separate cells.

Each call copies the source into a scratch folder and runs the notebook once with `App.run()` in a child Python process, which stays in the worker's process group: failure, cancellation, the deadline and stopping the project end it. The Card does not call `marimo export` afterwards, because the [export command runs the notebook again](https://docs.marimo.io/guides/exporting/static_html/) and could repeat side effects. Reactive dependencies order the cells inside that one execution; cells never send Card outputs or start downstream Cards. There is no marimo server, browser or interactive session. While it works, the Card shows the selected values in its `markdown` view, which is live and not saved.

## Saved results and source

The `report` Directory is retained as an artifact and contains:

- `index.html`: the static cell outputs, the selected values and the source cells with their names and line numbers;
- `result.json` and `bindings.json`: the selected definitions and the values supplied;
- `metadata.json`: the notebook source path;
- `source/`: the exact notebook and adapter source of that call.

The HTML holds ordinary Markdown and HTML, text and Matplotlib figures as embedded images. It needs no JavaScript, CDN or kernel. Frontend-only widgets are replaced by a note pointing to the saved source: this is a snapshot, not marimo's interactive frontend. Both clients preview it with scripts disabled, also after the project is reopened. Editing a downloaded copy does not change the recorded result.

A notebook error fails the Card and returns no output. The log lists the cells and their source lines before execution, then the Python traceback. The failure names the failed cell and its line; open the invocation's **Info** tab and choose the cell link to open that line in the notebook source.

In the bilingual demo, the upstream Card supplies a measurement of 7. The notebook scales it by `factor` = 2, draws it, and returns 14, which the `result` Card prints and shows. Set the notebook Card's `factor` to 3 and rerun to save a result of 21 next to the earlier one.

Interactive notebook sessions are tracked in [#267](https://github.com/gen740/tsugumi/issues/267); they are outside the committed invocation described here.
