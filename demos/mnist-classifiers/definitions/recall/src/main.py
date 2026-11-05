"""Per-digit recall of every model: which digit is hard, and for whom."""
import matplotlib.pyplot as plt
import numpy as np

from inputs.comparison.mnist.Comparison import Comparison


def main(ctx):
    c = ctx.input("comparison", Comparison)
    models = [c.Models(i).decode() for i in range(c.ModelsLength())]
    accuracy = c.AccuracyAsNumpy()
    labels = c.TestLabelsAsNumpy()
    predictions = c.PredictionsAsNumpy().reshape(len(models), len(labels))

    order = np.argsort(-accuracy, kind="stable")
    recall = np.array([[np.mean(predictions[m][labels == d] == d) for d in range(10)] for m in order])
    mean_by_digit = recall.mean(axis=0)
    hardest, easiest = np.argmin(mean_by_digit), np.argmax(mean_by_digit)
    print("mean recall by digit:", np.round(mean_by_digit, 4).tolist())

    fig, ax = plt.subplots(figsize=(8, 4.4), layout="constrained")
    image = ax.imshow(recall, cmap="viridis", vmin=recall.min(), vmax=1, aspect="auto")
    for i in range(recall.shape[0]):
        for j in range(10):
            mid = (recall.min() + 1) / 2
            ax.text(j, i, f"{recall[i, j]:.3f}".lstrip("0"), ha="center", va="center", fontsize=9,
                    color="black" if recall[i, j] > mid else "white")
    ax.set_xticks(range(10), labels=range(10))
    ax.set_yticks(range(len(order)), labels=[models[m] for m in order])
    ax.set_xlabel("正解の数字")
    ax.tick_params(length=0)
    ax.set_title(f"数字ごとの再現率。難しいのは {hardest}（平均 {mean_by_digit[hardest]:.3f}）、"
                 f"易しいのは {easiest}（平均 {mean_by_digit[easiest]:.3f}）", fontsize=11)
    fig.colorbar(image, ax=ax, shrink=0.8, label="再現率")
    plt.show()
