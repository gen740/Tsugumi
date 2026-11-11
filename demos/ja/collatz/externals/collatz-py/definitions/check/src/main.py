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

import outputs.value.collatz.Number as value_out
import outputs.even.collatz.Parity as even_out


def main(ctx):
    _, n = arrival(ctx)
    if n == 0:
        raise Failure("output_invalid", "Collatz is defined for positive integers")
    if n == 1:
        return  # no outputs: the loop ends
    b = ctx.builder()
    value_out.Start(b)
    value_out.AddValue(b, n)
    ctx.output("value", b, value_out.End(b))
    b = ctx.builder()
    even_out.Start(b)
    even_out.AddValue(b, n % 2 == 0)
    ctx.output("even", b, even_out.End(b))
