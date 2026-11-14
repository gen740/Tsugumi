"""A synthetic fluorescence-like image with known cell centres: blobs with soft
edges, touching pairs, uneven illumination and noise."""
import matplotlib.pyplot as plt
import numpy as np

from tsugumi import Failure


def place(rng, size, cells, pairs, radius):
    """Centres: touching pairs first, then single cells, closer together when
    the image gets too full to keep them apart."""
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


def main(inputs, report):
    p = inputs.params
    size, cells, pairs = p.size, p.cells, p.touchingPairs
    rng = np.random.default_rng(p.seed)
    centres = place(rng, size, cells, pairs, p.radius)
    image = render(rng, size, centres, p.radius, p.noise, p.unevenness)
    report.log(f"seed {p.seed}: {len(centres)} cells, {pairs} touching pairs, noise {p.noise}, unevenness {p.unevenness}")

    fig, axes = plt.subplots(1, 2, figsize=(9, 4.8))
    high = np.percentile(image, 99.5)
    for ax in axes:
        ax.imshow(image, cmap="gray", vmin=0, vmax=high)
        ax.axis("off")
    axes[0].set_title("合成画像（synthetic）")
    axes[1].plot(*centres[2 * pairs:].T, "+", color="#1baf7a", ms=7, label="離れた細胞")
    axes[1].plot(*centres[:2 * pairs].T, "+", color="#eb6834", ms=7, label="接した組の細胞")
    axes[1].set_title(f"真の中心: {len(centres)} 個（うち {pairs} 組が接している）")
    axes[1].legend(frameon=False, loc="upper center", bbox_to_anchor=(0.5, 0.0), ncol=2)
    fig.tight_layout()
    report.figure()
    image_v = image.astype(np.float32).ravel()
    x_v = centres[:, 0].astype(np.float32)
    y_v = centres[:, 1].astype(np.float32)
    return {'scene': {
        'size': size,
        'image': image_v,
        'true_x': x_v,
        'true_y': y_v,
        'radius': p.radius,
        'pairs': pairs,
        'seed': p.seed,
        'noise': p.noise,
        'unevenness': p.unevenness,
    }}
