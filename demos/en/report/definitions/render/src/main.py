"""Writes report.md and summary.json into a Directory output."""
import tsugumi
import json
import os

from inputs.config.report.Config import Config
from inputs.summary.report.Summary import Summary


def main(inputs, report, run):
    s = inputs.input("summary", Summary)
    heading = inputs.input("config", Config).Heading().decode()
    regions = [s.Regions(i) for i in range(s.RegionsLength())]
    lines = [f"# {heading}", "", f"{s.Rows()} sales, revenue {s.Revenue():,}", "",
             "| Region | Units | Revenue |", "| --- | ---: | ---: |"]
    data = []
    for r in regions:
        name = r.Region().decode()
        lines.append(f"| {name} | {r.Units():,} | {r.Revenue():,} |")
        data.append({"region": name, "units": r.Units(), "revenue": r.Revenue()})
    document = "\n".join(lines) + "\n"
    out = os.path.join(run.output_directory, "report")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write(document)
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump({"heading": heading, "rows": s.Rows(), "revenue": s.Revenue(), "regions": data}, f, indent=2)
    report.text(document)
    p = out
    _dir_out_value = {}
    _dir_out_value['value'] = p
    return {'report': _dir_out_value}
