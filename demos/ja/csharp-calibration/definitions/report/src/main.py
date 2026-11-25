from tsugumi import Markdown, send
from card import Artifact, Input, Output
from pathlib import Path


def run(inputs: Input) -> Output:
    artifact, directory = Artifact.dir()
    readings = inputs.numbers
    values = [readings.values[i] for i in range(len(readings.values))]
    document = Path(directory) / "calibrated.csv"
    document.write_text("sample,value\n" + "".join(f"{i},{value:.12g}\n" for i, value in enumerate(values, 1)))
    send(Markdown(f"**{len(values)} readings** · sum {sum(values):.6g} · mean {sum(values) / len(values):.6g}"))
    return Output(report=artifact)
