"""Writes a deterministic sales table: the same seed gives the same CSV."""
import csv
import os
import random

from inputs.config.report.Config import Config
import outputs.csv.report.CsvFile as csv_out

REGIONS = ["North", "South", "East", "West"]
PRODUCTS = ["Kettle", "Lamp", "Chair", "Desk", "Shelf"]


def main(ctx):
    config = ctx.input("config", Config)
    rng = random.Random(config.Seed())
    path = os.path.join(ctx.output_directory, "sales.csv")
    with open(path, "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["region", "product", "units", "unit_price"])
        for _ in range(config.Rows()):
            out.writerow([rng.choice(REGIONS), rng.choice(PRODUCTS), rng.randint(1, 20), rng.randint(5, 120)])
    print(f"wrote {config.Rows()} rows with seed {config.Seed()} to {path}")
    b = ctx.builder()
    p = b.CreateString(path)
    csv_out.Start(b)
    csv_out.AddValue(b, p)
    ctx.output("csv", b, csv_out.End(b))
