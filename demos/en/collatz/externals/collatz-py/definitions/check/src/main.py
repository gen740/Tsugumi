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



def main(inputs):
    result_ports = {}
    _, n = arrival(inputs)
    if n == 0:
        raise Failure("output_invalid", "Collatz is defined for positive integers")
    if n == 1:
        return  # no outputs: the loop ends
    _value_out_value = {}
    _value_out_value['value'] = n
    result_ports.update({'value': _value_out_value})
    _even_out_value = {}
    _even_out_value['value'] = n % 2 == 0
    result_ports.update({'even': _even_out_value})
    return result_ports
