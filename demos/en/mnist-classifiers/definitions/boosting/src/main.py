"""Gradient boosting: each round adds small trees that correct what the earlier ones got wrong."""
import os

# One thread: the numbers repeat and the times compare. Before the imports: the libraries read it as they load.
os.environ["OMP_NUM_THREADS"] = "1"

import time

import matplotlib.pyplot as plt
import numpy as np
import xgboost as xgb


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


def result(name, settings, digest, y, pred, fit, predict):
    model = name
    predictions = pred.astype(np.uint8)
    return {'result': {
        'model': model,
        'settings': settings,
        'digest': digest,
        'accuracy': float(np.mean(pred == y)),
        'fit_seconds': fit,
        'predict_seconds': predict,
        'predictions': predictions,
    }}


def main(inputs, report):
    params = inputs.params
    rounds, max_depth, eta, seed = params.rounds, params.maxDepth, params.eta, params.seed
    data = np.load(inputs.data.value.decode())
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
    report.log(f"boosting rounds={rounds} depth={max_depth} eta={eta}: accuracy {accuracy:.4f}, fit {fit:.3f} s, predict {predict:.3f} s")

    # The test error after each round, read back from the finished model (outside the timers).
    upto = np.unique(np.geomspace(1, rounds, 20).astype(int))
    errors = [np.mean(model.predict(test, iteration_range=(0, int(i))).argmax(axis=1) != test_y) for i in upto]

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4.2), layout="constrained")
    confusions(left, test_y, pred)
    right.plot(upto, errors, marker="o", markersize=3, color="#c0392b")
    right.set(xlabel="Rounds", ylabel="Test error rate", ylim=(0, None), title="More rounds, fewer mistakes")
    right.grid(alpha=0.3)
    fig.suptitle(f"Gradient boosting, {rounds} rounds, depth {max_depth}, η={eta:g}: accuracy {accuracy:.4f}, training {fit:.2f} s, prediction {predict:.3f} s", fontsize=11)
    report.figure()

    return result('boosting', f'rounds={rounds}, depth={max_depth}, eta={eta:g}', str(data['digest']), test_y, pred, fit, predict)
