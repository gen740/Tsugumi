"""Totals revenue per region. `fail_on_purpose` demonstrates a failing step."""
import tsugumi
import csv
import sys
from collections import defaultdict

from inputs.csv.report.CsvFile import CsvFile
from tsugumi import Failure


def main(inputs, report):
    if inputs.params.failOnPurpose:
        report.log(" ".join(str(value) for value in ('aggregate: failing on purpose (turn off fail_on_purpose and re-run this Card)',)))
        raise Failure('worker_failed', 'fail_on_purpose is enabled')
    path = inputs.input('csv', CsvFile).Value().decode()
    revenue = defaultdict(int)
    units = defaultdict(int)
    rows = 0
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            rows += 1
            u, price = int(row['units']), int(row['unit_price'])
            revenue[row["region"]] += u * price
            units[row["region"]] += u
    offsets = []
    for region in sorted(revenue):
        name = region
        _region_out_value = {}
        _region_out_value['region'] = name
        _region_out_value['revenue'] = revenue[region]
        _region_out_value['units'] = units[region]
        offsets.append(_region_out_value)
    _b_items = []
    for o in reversed(offsets):
        _b_items.append(o)
    regions = list(reversed(_b_items))
    _summary_out_value = {}
    _summary_out_value['rows'] = rows
    _summary_out_value['revenue'] = sum(revenue.values())
    _summary_out_value['regions'] = regions
    return {'summary': _summary_out_value}
