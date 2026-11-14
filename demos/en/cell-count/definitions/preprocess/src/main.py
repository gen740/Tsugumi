"""Background subtraction (a grey opening wider than a cell) and Gaussian smoothing."""
import tsugumi
import matplotlib.pyplot as plt
import numpy as np
from scipy import ndimage as ndi
from skimage.filters import gaussian

from inputs.scene.cells.Scene import Scene


def background(image, radius):
    """Opening on a quarter-size image, which is much faster and as good for a smooth background."""
    small = ndi.zoom(image, 0.25, order=1)
    opened = ndi.grey_opening(ndi.gaussian_filter(small, 1), size=(radius // 2, radius // 2))
    smooth = ndi.gaussian_filter(opened, radius / 4)
    return ndi.zoom(smooth, np.array(image.shape) / np.array(smooth.shape), order=1)


def main(inputs, report):
    scene = inputs.input("scene", Scene)
    p = inputs.params
    size = scene.Size()
    image = scene.ImageAsNumpy().reshape(size, size).astype(np.float64)
    back = background(image, p.backgroundRadius)
    clean = gaussian(image - back, sigma=p.sigma)
    report.log(str(f"background {back.min():.3f}..{back.max():.3f}; cleaned image {clean.min():.3f}..{clean.max():.3f}"))

    fig, axes = plt.subplots(1, 3, figsize=(11, 4))
    high = np.percentile(image, 99.5)
    shown = ((image, f"Original (noise {scene.Noise():g})", high),
             (back, f"Estimated background {back.min():.2f} – {back.max():.2f}", high),
             (clean, f"Subtracted and blurred (σ = {p.sigma:g})", np.percentile(clean, 99.5)))
    for ax, (data, title, top) in zip(axes, shown):
        ax.imshow(data, cmap="gray", vmin=0, vmax=top)
        ax.set_title(title, fontsize=11)
        ax.axis("off")
    fig.tight_layout()
    report.figure()
    image_v = clean.astype(np.float32).ravel()
    _out_value = {}
    _out_value['size'] = size
    _out_value['image'] = image_v
    return {'cleaned': _out_value}
