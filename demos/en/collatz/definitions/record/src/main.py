"""Writes the sequence a_0 ... a_k to collatz.csv as an Artifact."""
from tsugumi import Markdown, send
from card import Artifact, Input, Output
import os


def run(inputs: Input) -> Output:
    artifact, directory = Artifact.dir()
    seq = inputs.sequence
    values = [seq.values[i] for i in range(len(seq.values))]
    path = os.path.join(directory, "collatz.csv")
    with open(path, "w") as f:
        f.write("k,a_k,parity\n")
        for k, v in enumerate(values):
            f.write(f"{k},{v},{'even' if v % 2 == 0 else 'odd'}\n")
    send(Markdown(f"Recorded {len(values)} values in the sequence CSV."))
    return Output(table=artifact)
