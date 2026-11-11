"""Writes the sequence a_0 ... a_k to collatz.csv (kept as a File artifact)."""
import os

from inputs.sequence.collatz.Sequence import Sequence
import outputs.table.collatz.CsvFile as file_out


def main(ctx):
    seq = ctx.input("sequence", Sequence)
    values = [seq.Values(i) for i in range(seq.ValuesLength())]
    path = os.path.join(ctx.output_directory, "collatz.csv")
    with open(path, "w") as f:
        f.write("k,a_k,parity\n")
        for k, v in enumerate(values):
            f.write(f"{k},{v},{'even' if v % 2 == 0 else 'odd'}\n")
    odd = sum(1 for v in values[:-1] if v % 2)
    ctx.display_markdown(
        f"`collatz.csv`: {len(values)} rows ($k = 0 \\ldots {len(values) - 1}$)\n\n"
        f"| | count |\n| --- | ---: |\n| $3n+1$ (odd) | {odd} |\n| $n/2$ (even) | {len(values) - 1 - odd} |\n"
    )
    b = ctx.builder()
    p = b.CreateString(path)
    file_out.Start(b)
    file_out.AddValue(b, p)
    ctx.output("table", b, file_out.End(b))
