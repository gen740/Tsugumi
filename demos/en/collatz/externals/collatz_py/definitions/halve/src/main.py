from card import Input, Output
from collatz.Number import NumberT
def run(inputs: Input) -> Output:
    n = inputs.number.value
    return Output(next=NumberT(value=n // 2))
