from card import Artifact, Input, Output
from pathlib import Path


def run(inputs: Input) -> Output:
    artifact, directory = Artifact.dir()
    readings = inputs.numbers
    values = [readings.values[i] for i in range(len(readings.values))]
    document = Path(directory) / "calibrated.csv"
    document.write_text("sample,value\n" + "".join(f"{i},{value:.12g}\n" for i, value in enumerate(values, 1)))
    # TODO(open): Port the demo display.
    return Output(report=artifact)
