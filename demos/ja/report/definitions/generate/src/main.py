"""Writes a deterministic sales table: the same seed gives the same CSV."""
from card import Artifact, Input, Output
import csv
import os
import random


REGIONS = ["North", "South", "East", "West"]
PRODUCTS = ["Kettle", "Lamp", "Chair", "Desk", "Shelf"]


def run(inputs: Input) -> Output:
    artifact, directory = Artifact.dir()
    config = inputs.config
    rng = random.Random(config.seed)
    path = os.path.join(directory, "sales.csv")
    with open(path, "w", newline="") as f:
        out = csv.writer(f)
        out.writerow(["region", "product", "units", "unit_price"])
        for _ in range(config.rows):
            out.writerow([rng.choice(REGIONS), rng.choice(PRODUCTS), rng.randint(1, 20), rng.randint(5, 120)])
    print(f"wrote {config.rows} rows with seed {config.seed} to {path}")
    return Output(csv=artifact)
