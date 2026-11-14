"""A random forest: the vote of many decision trees, each grown on a random draw of the digits."""
import os

# One thread: the numbers repeat and the times compare. Before the imports: the libraries read it as they load.
os.environ["OMP_NUM_THREADS"] = "1"

import time

import matplotlib.pyplot as plt
import numpy as np
from sklearn.ensemble import RandomForestClassifier


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
    trees, max_depth, seed = params.trees, params.maxDepth, params.seed
    data = np.load(inputs.data.value.decode())
    train_x, train_y, test_x, test_y = data["train_images"], data["train_labels"], data["test_images"], data["test_labels"]

    model = RandomForestClassifier(n_estimators=trees, max_depth=max_depth, random_state=seed, n_jobs=1)
    start = time.process_time()
    model.fit(train_x, train_y)
    fit = time.process_time() - start
    start = time.process_time()
    pred = model.predict(test_x)
    predict = time.process_time() - start
    accuracy = np.mean(pred == test_y)
    report.log(f"forest trees={trees} depth={max_depth}: accuracy {accuracy:.4f}, fit {fit:.3f} s, predict {predict:.3f} s")

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4.2), layout="constrained")
    confusions(left, test_y, pred)
    right.imshow(model.feature_importances_.reshape(28, 28), cmap="magma")
    right.set(xticks=[], yticks=[], title="判定に効いた画素（明るいほど重要）")
    fig.suptitle(f"ランダムフォレスト {trees} 本、深さ {max_depth} まで: 正解率 {accuracy:.4f}、学習 {fit:.2f} 秒、予測 {predict:.2f} 秒", fontsize=11)
    report.figure()

    return result('forest', f'trees={trees}, depth={max_depth}', str(data['digest']), test_y, pred, fit, predict)
