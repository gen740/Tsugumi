"""k nearest neighbours: a test digit takes the majority label of the k closest training digits."""
import os

# One thread: the numbers repeat and the times compare. Before the imports: the libraries read it as they load.
os.environ["OMP_NUM_THREADS"] = "1"

import time

import matplotlib.pyplot as plt
import numpy as np
from sklearn.neighbors import KNeighborsClassifier


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
    k = inputs.params.k
    data = np.load(inputs.data.value.decode())
    train_x, train_y, test_x, test_y = data["train_images"], data["train_labels"], data["test_images"], data["test_labels"]

    model = KNeighborsClassifier(n_neighbors=k, algorithm="brute")
    start = time.process_time()
    model.fit(train_x, train_y)
    fit = time.process_time() - start
    start = time.process_time()
    pred = model.predict(test_x)
    predict = time.process_time() - start
    accuracy = np.mean(pred == test_y)
    report.log(f"knn k={k}: accuracy {accuracy:.4f}, fit {fit:.3f} s, predict {predict:.3f} s")

    # What it did on a few mistakes: the test digit, then its nearest training digits.
    wrong = np.flatnonzero(pred != test_y)[:6]
    shown = min(k, 4)
    neighbours = model.kneighbors(test_x[wrong], n_neighbors=shown, return_distance=False)
    rows = [np.hstack([test_x[w].reshape(28, 28)] + [train_x[n].reshape(28, 28) for n in ns])
            for w, ns in zip(wrong, neighbours)]

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4.2), layout="constrained")
    confusions(left, test_y, pred)
    if rows:
        right.imshow(np.vstack(rows), cmap="gray_r")
    for row, w in enumerate(wrong):
        right.text(-3, 28 * row + 14, f"true {test_y[w]} → {pred[w]}", ha="right", va="center", fontsize=8)
    right.set_xticks(28 * np.arange(1 + shown) + 14, labels=["Test"] + [f"Neighbour {i + 1}" for i in range(shown)], fontsize=8)
    right.set_yticks([])
    right.tick_params(length=0)
    right.set_title("Digits it got wrong, and their neighbours")
    fig.suptitle(f"k-nearest neighbours k={k}: accuracy {accuracy:.4f}, training {fit:.2f} s, prediction {predict:.2f} s", fontsize=11)
    report.figure()

    return result('knn', f'k={k}', str(data['digest']), test_y, pred, fit, predict)
