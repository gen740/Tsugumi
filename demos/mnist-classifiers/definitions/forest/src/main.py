"""A random forest: the vote of many decision trees, each grown on a random draw of the digits."""
import time

import matplotlib.pyplot as plt
import numpy as np
from threadpoolctl import threadpool_limits
from sklearn.ensemble import RandomForestClassifier

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
    threadpool_limits(1)  # one thread: the numbers repeat and the times compare
    params = ctx.params()
    trees, max_depth, seed = params.Trees(), params.MaxDepth(), params.Seed()
    data = np.load(ctx.input("data", SubsetFile).Value().decode())
    train_x, train_y, test_x, test_y = data["train_images"], data["train_labels"], data["test_images"], data["test_labels"]

    model = RandomForestClassifier(n_estimators=trees, max_depth=max_depth, random_state=seed, n_jobs=1)
    start = time.process_time()
    model.fit(train_x, train_y)
    fit = time.process_time() - start
    start = time.process_time()
    pred = model.predict(test_x)
    predict = time.process_time() - start
    accuracy = np.mean(pred == test_y)
    print(f"forest trees={trees} depth={max_depth}: accuracy {accuracy:.4f}, fit {fit:.3f} s, predict {predict:.3f} s")

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4.2), layout="constrained")
    confusions(left, test_y, pred)
    right.imshow(model.feature_importances_.reshape(28, 28), cmap="magma")
    right.set(xticks=[], yticks=[], title="判定に効いた画素（明るいほど重要）")
    fig.suptitle(f"ランダムフォレスト {trees} 本、深さ {max_depth} まで: 正解率 {accuracy:.4f}、学習 {fit:.2f} 秒、予測 {predict:.2f} 秒", fontsize=11)
    plt.show()

    send(ctx, "forest", f"trees={trees}, depth={max_depth}", str(data["digest"]), test_y, pred, fit, predict)
