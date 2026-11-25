from card import Input, Output, params
from sample.Number import NumberT
from tsugumi import Markdown, send


def run(_input: Input) -> Output:
    value = params.value
    send(Markdown(f"**Python**: {value}"))
    return Output(out=NumberT(value=value))
