"""Writes report.md and summary.json into a Directory output."""
import json
import os


def main(inputs, report, run):
    s = inputs.summary
    heading = inputs.config.heading.decode()
    regions = [s.regions[i] for i in range(len(s.regions))]
    lines = [f"# {heading}", "", f"{s.rows} sales, revenue {s.revenue:,}", "",
             "| Region | Units | Revenue |", "| --- | ---: | ---: |"]
    data = []
    for r in regions:
        name = r.region.decode()
        lines.append(f"| {name} | {r.units:,} | {r.revenue:,} |")
        data.append({"region": name, "units": r.units, "revenue": r.revenue})
    document = "\n".join(lines) + "\n"
    out = os.path.join(run.output_directory, "report")
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write(document)
    with open(os.path.join(out, "summary.json"), "w") as f:
        json.dump({"heading": heading, "rows": s.rows, "revenue": s.revenue, "regions": data}, f, indent=2)
    report.text(document)
    p = out
    return {'report': {'value': p}}
