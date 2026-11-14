"""Writes a deterministic sales table: the same seed gives the same CSV."""
import tsugumi
import csv
import os
import random

from inputs.config.report.Config import Config

REGIONS = ["North", "South", "East", "West"]
PRODUCTS = ["Kettle", "Lamp", "Chair", "Desk", "Shelf"]


def main(inputs, report, run):
    config = inputs.input("config", Config)
    rng = random.Random(config.Seed())
    path = os.path.join(run.output_directory, "sales.csv")
    with open(path, "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["region", "product", "units", "unit_price"])
        for _ in range(config.Rows()):
            out.writerow([rng.choice(REGIONS), rng.choice(PRODUCTS), rng.randint(1, 20), rng.randint(5, 120)])
    report.log(str(f"wrote {config.Rows()} rows with seed {config.Seed()} to {path}"))
    p = path
    _csv_out_value = {}
    _csv_out_value['value'] = p
    return {'csv': _csv_out_value}
