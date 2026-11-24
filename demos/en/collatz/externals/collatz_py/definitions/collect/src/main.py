from card import Input, Output
from collatz.Sequence import SequenceT

values = []


def run(input: Input) -> Output:
    arrival = input.require("arrival")
    if hasattr(arrival, "seed"):
        values.clear()
        values.append(arrival.seed)
    else:
        values.append(arrival.value)
    # TODO(open): Port the demo display.
    return Output(sequence=SequenceT(values=list(values))) if values[-1] == 1 else Output()
