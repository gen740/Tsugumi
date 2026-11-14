from inputs.arrival.tsugumi.merge.Arrival import Arrival

from tsugumi import Failure


def arrival(inputs):
    """Return the arrival port and the number carried by its union member."""
    m = inputs.arrival
    if m.valueType not in (Arrival.init, Arrival.half, Arrival.triple):
        raise Failure("output_invalid", "unexpected arrival")
    return m.valueType, m.value.value

values = []  # kept by the worker (this Card sets reuseWorker: true); each run starts empty


def on_run_start(inputs):
    values.clear()


def main(inputs, report):
    port, n = arrival(inputs)
    if port == Arrival.init:
        values.clear()
    values.append(n)
    report.text(f"{len(values)} values: " + " ".join(map(str, values)))
    if n != 1:
        return
    vector = list(values)
    return {'sequence': {'values': vector}}
