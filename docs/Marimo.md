# marimo notebook Cards

> This page describes the planned notebook Card behavior. See [demo status](Demos.md#demo-status).

Open the **marimo notebook → typed result** demo for a complete notebook Card. It is an ordinary Python Card: `src/analysis.py` is the editable notebook, `src/main.py` maps the Card's input to notebook variables and returns the result. `default.nix` takes marimo and Matplotlib from the project’s locked `<nixpkgs>` source (`packages = ps: [ ps.marimo ps.matplotlib ];`) and the Python SDK supplies the kit's IPAexGothic as Matplotlib's Japanese fallback font. Nothing is installed when a notebook runs or when a result is opened.

## One committed invocation

Connect a typed value to `data`. The Card's parameters say what goes in and what comes out:

| Parameter | Meaning |
| --- | --- |
| `notebook` | A `.py` notebook inside the Card's source directory, default `analysis.py` |
| `bindings` | JSON mapping notebook variables to `inputs.<port>.<field>` or `param.<field>` paths |
| `selected` | The notebook variable returned on the `value` output |
| `factor` | An ordinary parameter of the example, available to the bindings |

The default bindings map `inputs.data.source` to the notebook variable `source` and `param.factor` to `factor`. The notebook computes `result`, and the Card returns it on `value` with the saved notebook folder on the `report` Artifact output. To return another type, edit `_outputs.fbs` and the end of `main.py`.

A binding replaces a variable the notebook defines, so the mapping must provide **all variables defined by an overridden cell**: this is the behaviour of [marimo's `App.run(defs=…)`](https://docs.marimo.io/api/app/). Put unrelated configuration in separate cells.

Each call loads the notebook from the Card's source directory and runs it once
with `App.run(defs=…)`. The Card returns the selected numeric value and an
Artifact with `index.html`, `result.json`, `bindings.json` and the notebook and
adapter source. The run log shows Python failures. TODO(open): Port the demo
display and the rendering of notebook cell outputs.

In the bilingual demo, the upstream Card supplies a measurement of 7. The notebook scales it by `factor` = 2, draws it, and returns 14, which the `result` Card prints and shows. Set the notebook Card's `factor` to 3 and rerun to save a result of 21 next to the earlier one.

Interactive notebook sessions are tracked in [#267](https://github.com/gen740/tsugumi/issues/267); they are outside the committed invocation described here.
