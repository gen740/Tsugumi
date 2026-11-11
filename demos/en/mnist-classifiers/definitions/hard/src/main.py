"""The test digits most models get wrong, with what those models said instead."""
import matplotlib.pyplot as plt
import numpy as np

from inputs.comparison.mnist.Comparison import Comparison

SHOWN = 32


def main(ctx):
    c = ctx.input("comparison", Comparison)
    n_models = c.ModelsLength()
    labels = c.TestLabelsAsNumpy()
    predictions = c.PredictionsAsNumpy().reshape(n_models, len(labels))
    images = c.TestImagesAsNumpy().reshape(len(labels), 28, 28)

    wrong = (predictions != labels).sum(axis=0)
    order = np.argsort(-wrong, kind="stable")[:SHOWN]
    counts = np.bincount(wrong, minlength=n_models + 1)
    print("test digits by the number of models wrong:", counts.tolist())

    columns = 8
    rows = int(np.ceil(len(order) / columns))
    fig, axes = plt.subplots(rows, columns, figsize=(8, 1.25 * rows + 0.9), layout="constrained")
    for ax in axes.ravel():
        ax.axis("off")
    for ax, i in zip(axes.ravel(), order):
        answers = np.bincount(predictions[predictions[:, i] != labels[i], i], minlength=10)
        ax.imshow(images[i], cmap="gray_r")
        ax.set_title(f"{labels[i]} → {answers.argmax()}  ({wrong[i]}/{n_models})" if wrong[i] else f"{labels[i]}", fontsize=8)
    fig.suptitle(f"Test images many models got wrong (true → the most common wrong answer, number of models wrong)\n"
                 f"Of {len(labels)} images, all {n_models} models got {counts[0]} right and all got {counts[n_models]} wrong",
                 fontsize=10)
    plt.show()
