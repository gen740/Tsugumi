from card import Input, Output
from collatz.Number import NumberT


def run(input: Input) -> Output:
    value = input.require("value").value
    number = NumberT(value=value)
    return Output(a=number) if value % 2 == 0 else Output(b=number)
