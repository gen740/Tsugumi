from card import Input, Output
from collatz.Number import NumberT
from tsugumi import Failure

UINT64_MAX = (1 << 64) - 1


def run(inputs: Input) -> Output:
    n = inputs.number.value
    result = 3 * n + 1
    if result > UINT64_MAX:
        raise Failure("output_invalid", f"integer overflow: 3*{n}+1 exceeds uint64")
    return Output(next=NumberT(value=result))
