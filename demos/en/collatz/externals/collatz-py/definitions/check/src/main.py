from inputs.arrival.tsugumi.merge.Arrival import Arrival

from tsugumi import Failure


def arrival(inputs):
    """Return the arrival port and the number carried by its union member."""
    m = inputs.arrival
    if m.valueType not in (Arrival.init, Arrival.half, Arrival.triple):
        raise Failure("output_invalid", "unexpected arrival")
    return m.valueType, m.value.value


def main(inputs):
    outputs = {}
    _, n = arrival(inputs)
    if n == 0:
        raise Failure("output_invalid", "Collatz is defined for positive integers")
    if n == 1:
        return  # no outputs: the loop ends
    outputs.update({'value': {'value': n}})
    outputs.update({'even': {'value': n % 2 == 0}})
    return outputs
