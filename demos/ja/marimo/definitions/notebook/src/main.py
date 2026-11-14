"""Map inputs/params to notebook variables, then commit outputs once."""
import tsugumi
import json
import math
from pathlib import Path
from tsugumi import Failure
from tsugumi.notebook import bindings, run as run_notebook


def main(inputs, report, run):
    result_ports = {}
    p = inputs.params
    selected = p.selected.decode()
    definitions = bindings(inputs, json.loads(p.bindings.decode()))
    values, folder = run_notebook(report, run, Path(__file__).parent, p.notebook.decode(), definitions, [selected])
    value = values[selected]
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise Failure("output_invalid", "The selected notebook result must be a finite number; edit the output schema/source for another type")
    _number_value = {}; _number_value['value'] = value
    result_ports.update({'value': _number_value})
    path = str(folder)
    _files_value = {}; _files_value['value'] = path
    result_ports.update({'report': _files_value})
    return result_ports
