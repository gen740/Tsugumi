"""Where the method fails: new synthetic images over density and noise, scored the
same way. The steps are a copy of generate, preprocess, segment and score, so
that one Card can repeat them; the test checks that this copy gives the score
Card's numbers at the settings of generate."""
import json

import matplotlib.pyplot as plt
import numpy as np
from scipy import ndimage as ndi
from scipy.optimize import linear_sum_assignment
from skimage.feature import peak_local_max
from skimage.filters import gaussian, threshold_otsu
from skimage.measure import label, regionprops
from skimage.morphology import remove_small_objects
from skimage.segmentation import watershed

from inputs.scene.cells.Scene import Scene
from tsugumi import Failure


def place(rng, size, cells, pairs, radius):
    margin = 1.5 * radius
    centres = np.empty((0, 2))

    def free(point, gap):
        return np.all(np.sum((centres - point) ** 2, axis=1) >= gap ** 2)

    pairs = min(pairs, cells // 2)
    for _ in range(100000):
        if len(centres) >= 2 * pairs:
            break
        a = rng.uniform(margin, size - margin, 2)
        angle = rng.uniform(0, 2 * np.pi)
        c = a + rng.uniform(1.5, 1.9) * radius * np.array([np.cos(angle), np.sin(angle)])
        if margin < c[0] < size - margin and margin < c[1] < size - margin and free(a, 3 * radius) and free(c, 3 * radius):
            centres = np.vstack([centres, a, c])
    gap = 2.4 * radius
    misses = 0
    while len(centres) < cells:
        p = rng.uniform(margin, size - margin, 2)
        if free(p, gap):
            centres = np.vstack([centres, p])
            misses = 0
        else:
            misses += 1
            if misses > 2000:
                gap *= 0.95
                misses = 0
            if gap < radius:
                raise Failure("output_invalid", f"{cells} cells of radius {radius} do not fit in a {size} px image")
    return centres


def render(rng, size, centres, radius, noise, unevenness):
    yy, xx = np.mgrid[0:size, 0:size].astype(float)
    cells = np.zeros((size, size))
    for x, y in centres:
        r = radius * rng.uniform(0.85, 1.15)
        stretch = rng.uniform(0.8, 1.2)
        angle = rng.uniform(0, np.pi)
        brightness = rng.uniform(0.6, 1.0)
        u = (xx - x) * np.cos(angle) + (yy - y) * np.sin(angle)
        v = -(xx - x) * np.sin(angle) + (yy - y) * np.cos(angle)
        cells += brightness / (1 + np.exp((np.sqrt((u / stretch) ** 2 + (v * stretch) ** 2) - r) / 1.2))
    gain = 1 + unevenness * ((xx / size - 0.5) + 0.5 * (yy / size - 0.5))
    glow = 0.8 * unevenness * np.exp(-((xx - 0.2 * size) ** 2 + (yy - 0.8 * size) ** 2) / (2 * (0.45 * size) ** 2))
    return cells * gain + glow + 0.05 + rng.normal(0, noise, (size, size))


def background(image, radius):
    small = ndi.zoom(image, 0.25, order=1)
    opened = ndi.grey_opening(ndi.gaussian_filter(small, 1), size=(radius // 2, radius // 2))
    smooth = ndi.gaussian_filter(opened, radius / 4)
    return ndi.zoom(smooth, np.array(image.shape) / np.array(smooth.shape), order=1)


def segment(image, p):
    clean = gaussian(image - background(image, p.backgroundRadius), sigma=p.sigma).astype(np.float32)
    mask = remove_small_objects(ndi.binary_fill_holes(clean > threshold_otsu(clean)), max_size=p.minArea)
    plain = label(mask)
    distance = ndi.gaussian_filter(ndi.distance_transform_edt(mask), 1)
    peaks = peak_local_max(distance, min_distance=p.minDistance, labels=plain, exclude_border=False)
    markers = np.zeros(image.shape, dtype=np.int32)
    markers[tuple(peaks.T)] = np.arange(1, len(peaks) + 1)
    return plain, watershed(-distance, markers, mask=mask)


def f1_score(lab, truth, gate):
    """F1, and how many objects were found."""
    found = np.array([r.centroid[::-1] for r in regionprops(lab)]).reshape(-1, 2)
    if len(found) == 0:
        return 0.0, 0
    d = np.linalg.norm(found[:, None] - truth[None], axis=2)
    rows, cols = linear_sum_assignment(np.where(d <= gate, d, 1e6))
    matched = int(np.sum(d[rows, cols] <= gate))
    return 2 * matched / (len(found) + len(truth)), len(found)


def main(inputs, report):
    scene = inputs.input("scene", Scene)
    p = inputs.params
    size, radius = scene.Size(), scene.Radius()
    counts = p.cellsAsNumpy
    noises = p.noisesAsNumpy
    base_cells = scene.TrueXLength()
    plain_f1 = np.zeros((len(noises), len(counts)))
    split_f1 = np.zeros((len(noises), len(counts)))
    split_count = np.zeros((len(noises), len(counts)), dtype=int)
    for i, noise in enumerate(noises):
        for j, cells in enumerate(counts):
            rng = np.random.default_rng(scene.Seed())
            pairs = round(scene.Pairs() * int(cells) / base_cells)  # the same share of touching cells
            truth = place(rng, size, int(cells), pairs, radius)
            image = render(rng, size, truth, radius, float(noise), scene.Unevenness()).astype(np.float32).astype(np.float64)
            plain, split = segment(image, p)
            plain_f1[i, j] = f1_score(plain, truth, radius)[0]
            split_f1[i, j], split_count[i, j] = f1_score(split, truth, radius)
    report.log(str("sweep " + json.dumps({"cells": counts.tolist(), "noises": noises.tolist(),
                                 "plain": plain_f1.tolist(), "split": split_f1.tolist(),
                                 "split_count": split_count.tolist()})))

    fig, axes = plt.subplots(1, 2, figsize=(10.5, 4.4), sharey=True)
    for ax, title, table in ((axes[0], "Threshold only", plain_f1), (axes[1], "watershed", split_f1)):
        image = ax.imshow(table, cmap="Blues", vmin=0, vmax=1, aspect="auto", origin="lower")
        ax.set_xticks(range(len(counts)), [str(c) for c in counts])
        ax.set_yticks(range(len(noises)), [f"{n:g}" for n in noises])
        ax.set_xlabel(f"Number of cells ({size} × {size} px)")
        ax.set_title(f"F1 of {title} (synthetic data)", fontsize=11)
        for i in range(len(noises)):
            for j in range(len(counts)):
                ax.text(j, i, f"{table[i, j]:.2f}", ha="center", va="center", fontsize=9,
                        color="white" if table[i, j] > 0.55 else "#333333")
    axes[0].set_ylabel("Standard deviation of the noise (1 = the brightest cell)")
    axes[0].yaxis.label.set_size(9)
    fig.subplots_adjust(bottom=0.24)
    fig.colorbar(image, ax=axes, shrink=0.85, label="F1")
    fig.text(0.46, 0.03, "Top left (few cells, strong noise): the threshold falls into the noise and overcounts. Right edge (too crowded): cells merge and are missed.", ha="center", fontsize=9)
    report.figure()
