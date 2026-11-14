import tsugumi
from inputs.number.collatz.Number import Number


def main(inputs):
    n = inputs.input("number", Number).Value()
    _next_out_value = {}
    _next_out_value['value'] = n // 2
    return {'next': _next_out_value}
