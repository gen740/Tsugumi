import tsugumi
from inputs.cpp.collatz.Sequence import Sequence as CppSequence
from inputs.julia.collatz.Sequence import Sequence as JuliaSequence
from inputs.python.collatz.Sequence import Sequence as PySequence
from inputs.r.collatz.Sequence import Sequence as RSequence

from tsugumi import Failure


def values(seq):
    return [seq.Values(i) for i in range(seq.ValuesLength())]


def main(inputs, report):
    result_ports = {}
    found = {
        "C++": values(inputs.input('cpp', CppSequence)),
        "Python": values(inputs.input('python', PySequence)),
        "Julia": values(inputs.input('julia', JuliaSequence)),
        "R": values(inputs.input('r', RSequence)),
    }
    a = found["C++"]
    if any(v != a for v in found.values()):
        raise Failure("output_invalid", "sequences differ: " + ", ".join(f"{n} {len(v)} values" for n, v in found.items()))
    report.markdown(
        f"**C++, Python, Julia and R agree**: {len(a)} values, $a_0 = {a[0]}$ reaches $1$ "
        f"after ${len(a) - 1}$ steps (peak $\\max_k a_k = {max(a)}$)."
    )
    _builder_items = []
    for v in reversed(a):
        _builder_items.append(v)
    vector = list(reversed(_builder_items))
    _seq_out_value = {}
    _seq_out_value['values'] = vector
    sequence = _seq_out_value
    _verdict_out_value = {}
    _verdict_out_value['equal'] = True
    _verdict_out_value['count'] = len(a)
    verdict = _verdict_out_value
    result_ports.update({'verdict': verdict})
    result_ports.update({'sequence': sequence})
    return result_ports
