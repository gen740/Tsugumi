from card import Input, Output
from t.Number import NumberT
from tsugumi import Markdown, send


def run(_input: Input) -> Output:
    value = 7
    send(Markdown(f"**Python**: {value}"))
    return Output(out=NumberT(value=value))
