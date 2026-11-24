from card import Input, Output
from collatz.Number import NumberT
from tsugumi import Failure


def run(input: Input) -> Output:
    arrival = input.require("arrival")
    n = arrival.seed if hasattr(arrival, "seed") else arrival.value
    if n == 0:
        raise Failure("output_invalid", "Collatz is defined for positive integers")
    if n == 1:
        return Output()
    return Output(value=NumberT(value=n))
