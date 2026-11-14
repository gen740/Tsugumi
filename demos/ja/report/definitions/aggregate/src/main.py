"""Totals revenue per region. `fail_on_purpose` demonstrates a failing step."""
import csv
from collections import defaultdict

from tsugumi import Failure


def main(inputs, report):
    if inputs.params.failOnPurpose:
        report.log("aggregate: failing on purpose (turn off fail_on_purpose and re-run this Card)")
        raise Failure('worker_failed', 'fail_on_purpose is enabled')
    path = inputs.csv.value.decode()
    revenue = defaultdict(int)
    units = defaultdict(int)
    rows = 0
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            rows += 1
            u, price = int(row['units']), int(row['unit_price'])
            revenue[row["region"]] += u * price
            units[row["region"]] += u
    regions = []
    for region in sorted(revenue):
        regions.append({
            'region': region,
            'revenue': revenue[region],
            'units': units[region],
        })
    return {'summary': {
        'rows': rows,
        'revenue': sum(revenue.values()),
        'regions': regions,
    }}
