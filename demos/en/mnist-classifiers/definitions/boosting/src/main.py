"""Gradient boosting: each round adds small trees that correct what the earlier ones got wrong."""
import os

# One thread: the numbers repeat and the times compare. Before the imports: the libraries read it as they load.
os.environ["OMP_NUM_THREADS"] = "1"

import time

import matplotlib.pyplot as plt
import numpy as np
import xgboost as xgb

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
    ax.set(xticks=range(10), yticks=range(10), xlabel="Predicted", ylabel="True", title="Breakdown of mistakes (images)")
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
    params = ctx.params()
    rounds, max_depth, eta, seed = params.Rounds(), params.MaxDepth(), params.Eta(), params.Seed()
    data = np.load(ctx.input("data", SubsetFile).Value().decode())
    train_x, train_y, test_x, test_y = data["train_images"], data["train_labels"], data["test_images"], data["test_labels"]

    settings = {"objective": "multi:softprob", "num_class": 10, "max_depth": max_depth, "eta": eta,
                "tree_method": "hist", "max_bin": 64, "nthread": 1, "seed": seed}
    train = xgb.DMatrix(train_x, label=train_y)
    test = xgb.DMatrix(test_x)
    start = time.process_time()
    model = xgb.train(settings, train, num_boost_round=rounds)
    fit = time.process_time() - start
    start = time.process_time()
    pred = model.predict(test).argmax(axis=1)
    predict = time.process_time() - start
    accuracy = np.mean(pred == test_y)
    print(f"boosting rounds={rounds} depth={max_depth} eta={eta}: accuracy {accuracy:.4f}, fit {fit:.3f} s, predict {predict:.3f} s")

    # The test error after each round, read back from the finished model (outside the timers).
    upto = np.unique(np.geomspace(1, rounds, 20).astype(int))
    errors = [np.mean(model.predict(test, iteration_range=(0, int(i))).argmax(axis=1) != test_y) for i in upto]

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4.2), layout="constrained")
    confusions(left, test_y, pred)
    right.plot(upto, errors, marker="o", markersize=3, color="#c0392b")
    right.set(xlabel="Rounds", ylabel="Test error rate", ylim=(0, None), title="More rounds, fewer mistakes")
    right.grid(alpha=0.3)
    fig.suptitle(f"Gradient boosting, {rounds} rounds, depth {max_depth}, η={eta:g}: accuracy {accuracy:.4f}, training {fit:.2f} s, prediction {predict:.3f} s", fontsize=11)
    plt.show()

    send(ctx, "boosting", f"rounds={rounds}, depth={max_depth}, eta={eta:g}", str(data["digest"]), test_y, pred, fit, predict)
