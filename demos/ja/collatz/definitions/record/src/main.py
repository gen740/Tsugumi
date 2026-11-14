"""Writes the sequence a_0 ... a_k to collatz.csv (kept as a File artifact)."""
import tsugumi
import os

from inputs.sequence.collatz.Sequence import Sequence


def main(inputs, report, run):
    seq = inputs.input("sequence", Sequence)
    values = [seq.Values(i) for i in range(seq.ValuesLength())]
    path = os.path.join(run.output_directory, "collatz.csv")
    with open(path, "w") as f:
        f.write("k,a_k,parity\n")
        for k, v in enumerate(values):
            f.write(f"{k},{v},{'even' if v % 2 == 0 else 'odd'}\n")
    odd = sum(1 for v in values[:-1] if v % 2)
    report.markdown(
        f"`collatz.csv`: {len(values)} rows ($k = 0 \\ldots {len(values) - 1}$)\n\n"
        f"| | count |\n| --- | ---: |\n| $3n+1$ (odd) | {odd} |\n| $n/2$ (even) | {len(values) - 1 - odd} |\n"
    )
    p = path
    _file_out_value = {}
    _file_out_value['value'] = p
    return {'table': _file_out_value}
