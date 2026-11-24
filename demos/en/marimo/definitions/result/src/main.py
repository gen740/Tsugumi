from card import Input, Output, params


def run(input: Input) -> Output:
    value = input.require("in")
    if params.format == "text":
        print(value.value)
    elif params.format == "markdown":
        field = getattr(value, params.markdown_field)
        print(field.decode() if isinstance(field, bytes) else field)
    else:
        raise ValueError("expected text or markdown")
    # TODO(open): Port the demo display.
    return Output()
