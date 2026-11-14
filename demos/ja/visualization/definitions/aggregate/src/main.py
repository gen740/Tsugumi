"""Mean of five deterministic measurements at each of 24 times."""
import tsugumi
import math


def main(inputs, report):
    result_ports = {}
    xs = list(range(24))
    ys = [sum(10 + math.sin(t / 3) + j / 10 for j in range(5)) / 5 + inputs.params.offset for t in xs]
    _b_items = []
    for x in reversed(xs): _b_items.append(x)
    x = list(reversed(_b_items))
    _b_items = []
    for y in reversed(ys): _b_items.append(y)
    y = list(reversed(_b_items))
    _output_value = {}; _output_value['x'] = x; _output_value['y'] = y
    result_ports.update({'data': _output_value})
    report.table(["time", "mean"], [[x, y] for x, y in zip(xs, ys)])
    return result_ports
