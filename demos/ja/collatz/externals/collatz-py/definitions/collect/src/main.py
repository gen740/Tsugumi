from inputs.arrival.tsugumi.merge.Merged import Merged
from inputs.arrival.tsugumi.merge.Arrival import Arrival
from inputs.arrival.tsugumi.merge.p0.collatz.Number import Number as InitNumber
from inputs.arrival.tsugumi.merge.p1.collatz.Number import Number as HalfNumber
from inputs.arrival.tsugumi.merge.p2.collatz.Number import Number as TripleNumber

from tsugumi import Failure

_CLASSES = {Arrival.init: InitNumber, Arrival.half: HalfNumber, Arrival.triple: TripleNumber}


def arrival(ctx):
    """Returns (port, value) of the merge union delivered by the loop."""
    m = ctx.input("arrival", Merged)
    cls = _CLASSES.get(m.ValueType())
    if cls is None:
        raise Failure("output_invalid", "unexpected arrival")
    table = m.Value()
    number = cls()
    number.Init(table.Bytes, table.Pos)
    return m.ValueType(), number.Value()

import outputs.sequence.collatz.Sequence as seq_out
from inputs.arrival.tsugumi.merge.Arrival import Arrival as _A

values = []  # kept by the worker (this Card sets reuseWorker: true); each run starts empty


def on_run_start(ctx):
    values.clear()


def main(ctx):
    port, n = arrival(ctx)
    if port == _A.init:
        values.clear()
    values.append(n)
    ctx.display_text(f"{len(values)} values: " + " ".join(map(str, values)))
    if n != 1:
        return
    b = ctx.builder()
    seq_out.StartValuesVector(b, len(values))
    for v in reversed(values):
        b.PrependUint64(v)
    vector = b.EndVector()
    seq_out.Start(b)
    seq_out.AddValues(b, vector)
    ctx.output("sequence", b, seq_out.End(b))
