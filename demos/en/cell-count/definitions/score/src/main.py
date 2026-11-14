"""Counts and scores both segmentations against the true centres, side by side."""
import json

import matplotlib.pyplot as plt
import numpy as np
from scipy.optimize import linear_sum_assignment
from skimage.measure import regionprops
from skimage.segmentation import find_boundaries

from inputs.labels.cells.Labels import Labels
from inputs.scene.cells.Scene import Scene


def match(found, truth, gate):
    """One-to-one pairs within `gate`, as many as possible, then as close as possible."""
    d = np.linalg.norm(found[:, None] - truth[None], axis=2)
    rows, cols = linear_sum_assignment(np.where(d <= gate, d, 1e6))
    keep = d[rows, cols] <= gate
    return rows[keep], cols[keep]


def main(inputs, report):
    scene = inputs.input("scene", Scene)
    labels = inputs.input("labels", Labels)
    size = scene.Size()
    image = scene.ImageAsNumpy().reshape(size, size)
    truth = np.column_stack([scene.TrueXAsNumpy(), scene.TrueYAsNumpy()])
    gate = scene.Radius()

    fig, axes = plt.subplots(1, 2, figsize=(10, 5.8))
    high = np.percentile(image, 99.5)
    document = {"cells": len(truth), "gate": gate, "true_x": truth[:, 0].round(2).tolist(), "true_y": truth[:, 1].round(2).tolist()}
    methods = (("plain", "Threshold only", labels.PlainAsNumpy()), ("split", "watershed", labels.SplitAsNumpy()))
    for ax, (key, title, flat) in zip(axes, methods):
        lab = flat.reshape(size, size)
        found = np.array([r.centroid[::-1] for r in regionprops(lab)]).reshape(-1, 2)
        rows, cols = match(found, truth, gate)
        hit = np.zeros(len(found), bool)
        hit[rows] = True
        seen = np.zeros(len(truth), bool)
        seen[cols] = True
        precision = len(rows) / max(len(found), 1)
        recall = len(rows) / len(truth)
        f1 = 2 * precision * recall / max(precision + recall, 1e-12)
        document[key] = {"count": len(found), "matched": len(rows), "precision": precision, "recall": recall, "f1": f1,
                       "found_x": found[:, 0].round(2).tolist(), "found_y": found[:, 1].round(2).tolist()}

        ax.imshow(image, cmap="gray", vmin=0, vmax=high)
        edges = find_boundaries(lab, mode="inner")
        ax.imshow(np.ma.masked_where(~edges, edges), cmap="winter", alpha=0.75, interpolation="nearest")
        ax.plot(*found[hit].T, ".", color="#1baf7a", ms=5, label=f"found correctly {len(rows)}")
        ax.plot(*found[~hit].T, "s", mfc="none", mec="#e87ba4", ms=8, mew=1.5, label=f"extra {int((~hit).sum())}")
        ax.plot(*truth[~seen].T, "x", color="#eb6834", ms=8, mew=2, label=f"missed {int((~seen).sum())}")
        ax.set_title(f"{title}: {len(found)} (error {len(found) - len(truth):+d})\n"
                     f"precision {precision:.2f}, recall {recall:.2f}, F1 {f1:.2f}", fontsize=11)
        ax.legend(frameon=False, loc="upper center", bbox_to_anchor=(0.5, 0.0), ncol=3, fontsize=9)
        ax.axis("off")
    fig.suptitle(f"Scored against the synthetic data's true count of {len(truth)} (matched within {gate:g} px)", fontsize=11)
    fig.tight_layout()
    report.figure()
    report.log(str("scores " + json.dumps(document)))
