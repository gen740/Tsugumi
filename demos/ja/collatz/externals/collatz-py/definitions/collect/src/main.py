import tsugumi
from inputs.arrival.tsugumi.merge.Merged import Merged
from inputs.arrival.tsugumi.merge.Arrival import Arrival
from inputs.arrival.tsugumi.merge.p0.collatz.Number import Number as InitNumber
from inputs.arrival.tsugumi.merge.p1.collatz.Number import Number as HalfNumber
from inputs.arrival.tsugumi.merge.p2.collatz.Number import Number as TripleNumber

from tsugumi import Failure

_CLASSES = {Arrival.init: InitNumber, Arrival.half: HalfNumber, Arrival.triple: TripleNumber}


def arrival(inputs):
    """Returns (port, value) of the merge union delivered by the loop."""
    m = inputs.input("arrival", Merged)
    cls = _CLASSES.get(m.ValueType())
    if cls is None:
        raise Failure("output_invalid", "unexpected arrival")
    table = m.Value()
    number = cls()
    number.Init(table.Bytes, table.Pos)
    return m.ValueType(), number.Value()

from inputs.arrival.tsugumi.merge.Arrival import Arrival as _A

values = []  # kept by the worker (this Card sets reuseWorker: true); each run starts empty


def on_run_start(inputs):
    values.clear()


def main(inputs, report):
    port, n = arrival(inputs)
    if port == _A.init:
        values.clear()
    values.append(n)
    report.text(f"{len(values)} values: " + " ".join(map(str, values)))
    if n != 1:
        return
    _b_items = []
    for v in reversed(values):
        _b_items.append(v)
    vector = list(reversed(_b_items))
    _seq_out_value = {}
    _seq_out_value['values'] = vector
    return {'sequence': _seq_out_value}
