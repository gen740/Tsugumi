"""Counts and scores both segmentations against the true centres, side by side."""
import json

import matplotlib.pyplot as plt
import numpy as np
from scipy.optimize import linear_sum_assignment
from skimage.measure import regionprops
from skimage.segmentation import find_boundaries


def match(found, truth, gate):
    """One-to-one pairs within `gate`, as many as possible, then as close as possible."""
    d = np.linalg.norm(found[:, None] - truth[None], axis=2)
    rows, cols = linear_sum_assignment(np.where(d <= gate, d, 1e6))
    keep = d[rows, cols] <= gate
    return rows[keep], cols[keep]


def main(inputs, report):
    scene = inputs.scene
    labels = inputs.labels
    size = scene.size
    image = scene.image.reshape(size, size)
    truth = np.column_stack([scene.trueX, scene.trueY])
    gate = scene.radius

    fig, axes = plt.subplots(1, 2, figsize=(10, 5.8))
    high = np.percentile(image, 99.5)
    document = {"cells": len(truth), "gate": gate, "true_x": truth[:, 0].round(2).tolist(), "true_y": truth[:, 1].round(2).tolist()}
    methods = (("plain", "閾値のみ", labels.plain), ("split", "watershed", labels.split))
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
        ax.plot(*found[hit].T, ".", color="#1baf7a", ms=5, label=f"正しく見つけた {len(rows)}")
        ax.plot(*found[~hit].T, "s", mfc="none", mec="#e87ba4", ms=8, mew=1.5, label=f"余分 {int((~hit).sum())}")
        ax.plot(*truth[~seen].T, "x", color="#eb6834", ms=8, mew=2, label=f"見逃した {int((~seen).sum())}")
        ax.set_title(f"{title}: {len(found)} 個（誤差 {len(found) - len(truth):+d}）\n"
                     f"適合率 {precision:.2f}、再現率 {recall:.2f}、F1 {f1:.2f}", fontsize=11)
        ax.legend(frameon=False, loc="upper center", bbox_to_anchor=(0.5, 0.0), ncol=3, fontsize=9)
        ax.axis("off")
    fig.suptitle(f"合成データの真の個数 {len(truth)} 個に対する採点（距離 {gate:g} px 以内で対応づけ）", fontsize=11)
    fig.tight_layout()
    report.figure()
    report.log("scores " + json.dumps(document))
