"""Map inputs/params to notebook variables, then commit outputs once."""
import json
import math
from pathlib import Path
from tsugumi import Failure
from tsugumi.notebook import bindings, run
import outputs.value.notebook.Number as number
import outputs.report.notebook.Files as files


def main(ctx):
    p = ctx.params()
    selected = p.Selected().decode()
    definitions = bindings(ctx, json.loads(p.Bindings().decode()))
    values, folder = run(ctx, Path(__file__).parent, p.Notebook().decode(), definitions, [selected])
    value = values[selected]
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise Failure("output_invalid", "The selected notebook result must be a finite number; edit the output schema/source for another type")
    b = ctx.builder()
    number.Start(b); number.AddValue(b, value)
    ctx.output("value", b, number.End(b))
    b = ctx.builder(); path = b.CreateString(str(folder))
    files.Start(b); files.AddValue(b, path)
    ctx.output("report", b, files.End(b))
