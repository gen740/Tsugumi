from tsugumi import Markdown, send
from card import Input, Output
from collatz.Sequence import SequenceT
from collatz.Verdict import VerdictT
from tsugumi import Failure


def run(inputs: Input) -> Output:
    found = {
        "C++": list(inputs.cpp.values),
        "Python": list(inputs.python.values),
        "Julia": list(inputs.julia.values),
        "R": list(inputs.r.values),
    }
    a = found["C++"]
    if any(v != a for v in found.values()):
        raise Failure("output_invalid", "sequences differ: " + ", ".join(f"{n} {len(v)} values" for n, v in found.items()))
    send(Markdown(f"**{', '.join(found)} agree** on {len(a)} values."))
    return Output(sequence=SequenceT(values=a), verdict=VerdictT(equal=True, count=len(a)))
