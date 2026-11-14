"""A random forest: the vote of many decision trees, each grown on a random draw of the digits."""
import tsugumi
import os

# One thread: the numbers repeat and the times compare. Before the imports: the libraries read it as they load.
os.environ["OMP_NUM_THREADS"] = "1"

import time

import matplotlib.pyplot as plt
import numpy as np
from sklearn.ensemble import RandomForestClassifier

from inputs.data.mnist.SubsetFile import SubsetFile


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


def send(inputs, name, settings, digest, y, pred, fit, predict):
    result_ports = {}
    model = name
    settings = settings
    digest = digest
    predictions = pred.astype(np.uint8)
    _out_value = {}
    _out_value['model'] = model
    _out_value['settings'] = settings
    _out_value['digest'] = digest
    _out_value['accuracy'] = float(np.mean(pred == y))
    _out_value['fit_seconds'] = fit
    _out_value['predict_seconds'] = predict
    _out_value['predictions'] = predictions
    result_ports.update({'result': _out_value})
    return result_ports


def main(inputs, report):
    result_ports = {}
    params = inputs.params
    trees, max_depth, seed = params.trees, params.maxDepth, params.seed
    data = np.load(inputs.input("data", SubsetFile).Value().decode())
    train_x, train_y, test_x, test_y = data["train_images"], data["train_labels"], data["test_images"], data["test_labels"]

    model = RandomForestClassifier(n_estimators=trees, max_depth=max_depth, random_state=seed, n_jobs=1)
    start = time.process_time()
    model.fit(train_x, train_y)
    fit = time.process_time() - start
    start = time.process_time()
    pred = model.predict(test_x)
    predict = time.process_time() - start
    accuracy = np.mean(pred == test_y)
    report.log(str(f"forest trees={trees} depth={max_depth}: accuracy {accuracy:.4f}, fit {fit:.3f} s, predict {predict:.3f} s"))

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4.2), layout="constrained")
    confusions(left, test_y, pred)
    right.imshow(model.feature_importances_.reshape(28, 28), cmap="magma")
    right.set(xticks=[], yticks=[], title="Pixels that mattered for the decision (brighter is more important)")
    fig.suptitle(f"Random forest, {trees} trees, depth up to {max_depth}: accuracy {accuracy:.4f}, training {fit:.2f} s, prediction {predict:.2f} s", fontsize=11)
    report.figure()

    result_ports.update(send(inputs, 'forest', f'trees={trees}, depth={max_depth}', str(data['digest']), test_y, pred, fit, predict))
    return result_ports
