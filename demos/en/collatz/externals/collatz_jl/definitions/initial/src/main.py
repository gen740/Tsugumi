from card import Input, Output, params
from collatz.Initial import InitialT


def run(_input: Input) -> Output:
    return Output(value=InitialT(seed=int(params.value, 10)))
