from card import Input, Output
from collatz.Walk import WalkT


def run(input: Input) -> Output:
    walk = input.require("value")
    result = WalkT(value=walk.value, path=walk.path)
    return Output(a=result) if walk.value % 2 == 0 else Output(b=result)
