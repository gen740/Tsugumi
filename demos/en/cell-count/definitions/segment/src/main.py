"""Otsu threshold, connected components, then a distance-transform watershed that
splits what the threshold left joined."""
import tsugumi
import matplotlib.pyplot as plt
import numpy as np
from scipy import ndimage as ndi
from skimage.color import label2rgb
from skimage.feature import peak_local_max
from skimage.filters import threshold_otsu
from skimage.measure import label
from skimage.morphology import remove_small_objects
from skimage.segmentation import watershed

from inputs.cleaned.cells.Cleaned import Cleaned


def main(inputs, report):
    cleaned = inputs.input("cleaned", Cleaned)
    p = inputs.params
    size = cleaned.Size()
    image = cleaned.ImageAsNumpy().reshape(size, size)

    level = threshold_otsu(image)
    mask = remove_small_objects(ndi.binary_fill_holes(image > level), max_size=p.minArea)
    plain = label(mask)
    distance = ndi.gaussian_filter(ndi.distance_transform_edt(mask), 1)
    peaks = peak_local_max(distance, min_distance=p.minDistance, labels=plain, exclude_border=False)
    markers = np.zeros(image.shape, dtype=np.int32)
    markers[tuple(peaks.T)] = np.arange(1, len(peaks) + 1)
    split = watershed(-distance, markers, mask=mask)
    report.log(str(f"Otsu level {level:.4f}; threshold only {plain.max()} objects; watershed {split.max()} objects"))

    fig, axes = plt.subplots(1, 3, figsize=(11, 4))
    rng = np.random.default_rng(0)
    shown = ((mask, f"Foreground at Otsu's threshold {level:.3f}", None),
             (plain, f"Threshold only: {plain.max()}", plain.max()),
             (split, f"watershed: {split.max()}", split.max()))
    for ax, (data, title, count) in zip(axes, shown):
        if count is None:
            ax.imshow(data, cmap="gray")
        else:
            ax.imshow(label2rgb(data, colors=rng.uniform(0.25, 1, (max(count, 1), 3)), bg_label=0))
        ax.set_title(title, fontsize=11)
        ax.axis("off")
    fig.tight_layout()
    report.figure()
    plain_v = plain.astype(np.int32).ravel()
    split_v = split.astype(np.int32).ravel()
    _out_value = {}
    _out_value['size'] = size
    _out_value['plain'] = plain_v
    _out_value['split'] = split_v
    return {'labels': _out_value}
