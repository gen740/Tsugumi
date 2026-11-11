"""Writes report.md and summary.json into a Directory output."""
import json
import os

from inputs.config.report.Config import Config
from inputs.summary.report.Summary import Summary
import outputs.report.report.ReportDir as dir_out


def main(ctx):
    s = ctx.input("summary", Summary)
    heading = ctx.input("config", Config).Heading().decode()
    regions = [s.Regions(i) for i in range(s.RegionsLength())]
    lines = [f"# {heading}", "", f"{s.Rows()} sales, revenue {s.Revenue():,}", "",
             "| Region | Units | Revenue |", "| --- | ---: | ---: |"]
    data = []
    for r in regions:
        name = r.Region().decode()
        lines.append(f"| {name} | {r.Units():,} | {r.Revenue():,} |")
        data.append({"region": name, "units": r.Units(), "revenue": r.Revenue()})
    report = "\n".join(lines) + "\n"
    out = os.path.join(ctx.output_directory, "report")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write(report)
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump({"heading": heading, "rows": s.Rows(), "revenue": s.Revenue(), "regions": data}, f, indent=2)
    ctx.display_text(report)
    b = ctx.builder()
    p = b.CreateString(out)
    dir_out.Start(b)
    dir_out.AddValue(b, p)
    ctx.output("report", b, dir_out.End(b))
