"""Totals revenue per region. `fail_on_purpose` demonstrates a failing step."""
import csv
import sys
from collections import defaultdict

from inputs.csv.report.CsvFile import CsvFile
import outputs.summary.report.RegionTotal as region_out
import outputs.summary.report.Summary as summary_out
from tsugumi import Failure


def main(ctx):
    if ctx.params().FailOnPurpose():
        print("aggregate: failing on purpose (turn off fail_on_purpose and re-run this Card)", file=sys.stderr)
        raise Failure("worker_failed", "fail_on_purpose is enabled")
    path = ctx.input("csv", CsvFile).Value().decode()
    revenue = defaultdict(int)
    units = defaultdict(int)
    rows = 0
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            rows += 1
            u, price = int(row["units"]), int(row["unit_price"])
            revenue[row["region"]] += u * price
            units[row["region"]] += u
    b = ctx.builder()
    offsets = []
    for region in sorted(revenue):
        name = b.CreateString(region)
        region_out.Start(b)
        region_out.AddRegion(b, name)
        region_out.AddRevenue(b, revenue[region])
        region_out.AddUnits(b, units[region])
        offsets.append(region_out.End(b))
    summary_out.StartRegionsVector(b, len(offsets))
    for o in reversed(offsets):
        b.PrependUOffsetTRelative(o)
    regions = b.EndVector()
    summary_out.Start(b)
    summary_out.AddRows(b, rows)
    summary_out.AddRevenue(b, sum(revenue.values()))
    summary_out.AddRegions(b, regions)
    ctx.output("summary", b, summary_out.End(b))
