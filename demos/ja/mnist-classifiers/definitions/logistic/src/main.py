"""Logistic regression: one weight per pixel for each digit, the largest weighted sum wins."""
import os

# One thread: the numbers repeat and the times compare. Before the imports: the libraries read it as they load.
os.environ["OMP_NUM_THREADS"] = "1"

import time

import matplotlib.pyplot as plt
import numpy as np
from sklearn.linear_model import LogisticRegression

from inputs.data.mnist.SubsetFile import SubsetFile
import outputs.result.mnist.Result as out


def confusions(ax, y, pred):
    """The mistakes only: true digit (rows) against predicted digit (columns)."""
    m = np.zeros((10, 10), dtype=int)
    np.add.at(m, (y, pred), 1)
    np.fill_diagonal(m, 0)
    ax.imshow(m, cmap="Reds")
    for i, j in zip(*np.nonzero(m)):
        ax.text(j, i, m[i, j], ha="center", va="center", fontsize=7, color="white" if m[i, j] > m.max() / 2 else "black")
    ax.set(xticks=range(10), yticks=range(10), xlabel="予測", ylabel="正解", title="間違いの内訳（枚数）")
    ax.tick_params(length=0, labelsize=8)


def send(ctx, name, settings, digest, y, pred, fit, predict):
    b = ctx.builder()
    model = b.CreateString(name)
    settings = b.CreateString(settings)
    digest = b.CreateString(digest)
    predictions = b.CreateNumpyVector(pred.astype(np.uint8))
    out.Start(b)
    out.AddModel(b, model)
    out.AddSettings(b, settings)
    out.AddDigest(b, digest)
    out.AddAccuracy(b, float(np.mean(pred == y)))
    out.AddFitSeconds(b, fit)
    out.AddPredictSeconds(b, predict)
    out.AddPredictions(b, predictions)
    ctx.output("result", b, out.End(b))


def main(ctx):
    c, max_iter = ctx.params().C(), ctx.params().MaxIter()
    data = np.load(ctx.input("data", SubsetFile).Value().decode())
    train_x, train_y, test_x, test_y = data["train_images"], data["train_labels"], data["test_images"], data["test_labels"]

    model = LogisticRegression(C=c, max_iter=max_iter)
    start = time.process_time()
    model.fit(train_x, train_y)
    fit = time.process_time() - start
    start = time.process_time()
    pred = model.predict(test_x)
    predict = time.process_time() - start
    accuracy = np.mean(pred == test_y)
    print(f"logistic C={c}: accuracy {accuracy:.4f}, {model.n_iter_[0]} iterations, fit {fit:.3f} s, predict {predict:.3f} s")

    weights = model.coef_.reshape(10, 28, 28)
    mosaic = np.vstack([np.hstack(weights[:5]), np.hstack(weights[5:])])
    limit = np.abs(mosaic).max()

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4.2), layout="constrained")
    confusions(left, test_y, pred)
    right.imshow(mosaic, cmap="RdBu_r", vmin=-limit, vmax=limit)
    for d in range(10):
        right.text(28 * (d % 5) + 2, 28 * (d // 5) + 4, str(d), fontsize=9)
    right.set(xticks=[], yticks=[], title="数字ごとの重み（赤: その数字らしい画素、青: 逆）")
    fig.suptitle(f"ロジスティック回帰 C={c:g}: 正解率 {accuracy:.4f}、学習 {fit:.2f} 秒、予測 {predict:.3f} 秒", fontsize=11)
    plt.show()

    send(ctx, "logistic", f"C={c:g}", str(data["digest"]), test_y, pred, fit, predict)
