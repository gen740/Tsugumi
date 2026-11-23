from card import Input, Output


def run(input: Input) -> Output:
    for name in ("rmarkdown", "quarto"):
        if (input.require(name).open() / "index.html").stat().st_size == 0:
            raise ValueError(f"{name} report is empty")
    # TODO(open): Port the demo display.
    return Output()
