"""The same steps on a real image of nuclei. There is no ground truth here, so it
shows the counts and the outlines to look at instead of a score."""
import matplotlib.pyplot as plt
import numpy as np
from scipy import ndimage as ndi
from skimage.feature import peak_local_max
from skimage.filters import gaussian, threshold_otsu
from skimage.io import imread
from skimage.measure import label, regionprops
from skimage.morphology import remove_small_objects
from skimage.segmentation import find_boundaries, watershed

from inputs.image.cells.RealImage import RealImage


def background(image, radius):
    small = ndi.zoom(image, 0.25, order=1)
    opened = ndi.grey_opening(ndi.gaussian_filter(small, 1), size=(radius // 2, radius // 2))
    smooth = ndi.gaussian_filter(opened, radius / 4)
    return ndi.zoom(smooth, np.array(image.shape) / np.array(smooth.shape), order=1)


def main(ctx):
    p = ctx.params()
    source = ctx.input("image", RealImage)
    raw = imread(source.Path().decode()).astype(np.float64)
    image = raw / np.percentile(raw, 99.9)
    clean = gaussian(image - background(image, p.BackgroundRadius()), sigma=p.Sigma())
    mask = remove_small_objects(ndi.binary_fill_holes(clean > threshold_otsu(clean)), max_size=p.MinArea())
    plain = label(mask)
    distance = ndi.gaussian_filter(ndi.distance_transform_edt(mask), 1)
    peaks = peak_local_max(distance, min_distance=p.MinDistance(), labels=plain, exclude_border=False)
    markers = np.zeros(image.shape, dtype=np.int32)
    markers[tuple(peaks.T)] = np.arange(1, len(peaks) + 1)
    split = watershed(-distance, markers, mask=mask)
    areas = [r.area for r in regionprops(split)]
    print(f"{raw.shape[1]} x {raw.shape[0]} px from {source.Source().decode()}")
    print(f"threshold only {plain.max()} objects; watershed {split.max()} objects; median area {np.median(areas):.0f} px")

    fig, axes = plt.subplots(1, 2, figsize=(9, 5.2))
    for ax in axes:
        ax.imshow(image, cmap="gray", vmin=0, vmax=0.6)
        ax.axis("off")
    edges = find_boundaries(split, mode="inner")
    axes[1].imshow(np.ma.masked_where(~edges, edges), cmap="autumn", interpolation="nearest")
    axes[0].set_title("実画像: ヒト培養細胞の核（Moffat et al. 2006, CC0）", fontsize=10)
    axes[1].set_title(f"閾値のみ {plain.max()} 個 → watershed {split.max()} 個", fontsize=10)
    fig.text(0.5, 0.02, "正解がないので採点はしません。接した核や、分裂中の明るい核を輪郭で確かめてください。",
             ha="center", fontsize=9)
    fig.tight_layout(rect=(0, 0.06, 1, 1))
    plt.show()
