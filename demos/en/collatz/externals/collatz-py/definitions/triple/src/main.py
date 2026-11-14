import tsugumi
from inputs.number.collatz.Number import Number

from tsugumi import Failure

UINT64_MAX = (1 << 64) - 1


def main(inputs):
    n = inputs.input("number", Number).Value()
    result = 3 * n + 1
    if result > UINT64_MAX:
        raise Failure("output_invalid", f"integer overflow: 3*{n}+1 exceeds uint64")
    _next_out_value = {}
    _next_out_value['value'] = result
    return {'next': _next_out_value}
