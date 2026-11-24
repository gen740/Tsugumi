"""Writes report.md and summary.json in an Artifact."""
from card import Artifact, Input, Output, params
import json
import os


def run(inputs: Input) -> Output:
    artifact, directory = Artifact.dir()
    s = inputs.summary
    heading = params.heading
    regions = [s.regions[i] for i in range(len(s.regions))]
    lines = [f"# {heading}", "", f"{s.rows} sales, revenue {s.revenue:,}", "",
             "| Region | Units | Revenue |", "| --- | ---: | ---: |"]
    data = []
    for r in regions:
        name = r.region.decode()
        lines.append(f"| {name} | {r.units:,} | {r.revenue:,} |")
        data.append({"region": name, "units": r.units, "revenue": r.revenue})
    document = "\n".join(lines) + "\n"
    out = directory
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write(document)
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump({"heading": heading, "rows": s.rows, "revenue": s.revenue, "regions": data}, f, indent=2)
    # TODO(open): Port the demo display.
    return Output(report=artifact)
