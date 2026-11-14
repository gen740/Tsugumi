"""Map inputs/params to notebook variables, then commit outputs once."""
import json
import math
from pathlib import Path
from tsugumi import Failure
from tsugumi.notebook import bindings, run as run_notebook


def main(inputs, report, run):
    outputs = {}
    p = inputs.params
    selected = p.selected.decode()
    definitions = bindings(inputs, json.loads(p.bindings.decode()))
    values, folder = run_notebook(report, run, Path(__file__).parent, p.notebook.decode(), definitions, [selected])
    value = values[selected]
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise Failure("output_invalid", "The selected notebook result must be a finite number; edit the output schema/source for another type")
    outputs.update({'value': {'value': value}})
    path = str(folder)
    outputs.update({'report': {'value': path}})
    return outputs
