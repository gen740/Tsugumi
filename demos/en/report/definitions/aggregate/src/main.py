"""Totals revenue per region. `fail_on_purpose` demonstrates a failing step."""
import csv
from collections import defaultdict

from card import Input, Output, params
from report.RegionTotal import RegionTotalT
from report.Summary import SummaryT
from tsugumi import Failure


def run(inputs: Input) -> Output:
    if params.fail_on_purpose:
        print("aggregate: failing on purpose (turn off fail_on_purpose and re-run this Card)")
        raise Failure('worker_failed', 'fail_on_purpose is enabled')
    path = inputs.require("csv").open() / "sales.csv"
    revenue = defaultdict(int)
    units = defaultdict(int)
    rows = 0
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            rows += 1
            u, price = int(row['units']), int(row['unit_price'])
            revenue[row["region"]] += u * price
            units[row["region"]] += u
    regions = [
        RegionTotalT(region=region, revenue=revenue[region], units=units[region])
        for region in sorted(revenue)
    ]
    # TODO(open): Port the demo display.
    return Output(summary=SummaryT(rows=rows, revenue=sum(revenue.values()), regions=regions))
