"""A support vector machine with an RBF kernel: decides by similarity to a few training digits."""
import os

# One thread: the numbers repeat and the times compare. Before the imports: the libraries read it as they load.
os.environ["OMP_NUM_THREADS"] = "1"

import time

import matplotlib.pyplot as plt
import numpy as np
from sklearn.svm import SVC


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
    c, gamma = inputs.params.c, inputs.params.gamma
    data = np.load(inputs.data.value.decode())
    train_x, train_y, test_x, test_y = data["train_images"], data["train_labels"], data["test_images"], data["test_labels"]

    model = SVC(C=c, gamma=gamma, kernel="rbf")
    start = time.process_time()
    model.fit(train_x, train_y)
    fit = time.process_time() - start
    start = time.process_time()
    pred = model.predict(test_x)
    predict = time.process_time() - start
    accuracy = np.mean(pred == test_y)
    report.log(f"svm C={c} gamma={gamma}: accuracy {accuracy:.4f}, {model.n_support_.sum()} support vectors, "
          f"fit {fit:.3f} s, predict {predict:.3f} s")

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4.2), layout="constrained")
    confusions(left, test_y, pred)
    train_counts = np.bincount(train_y, minlength=10)
    right.bar(range(10), train_counts, color="#d9d9d9", label="訓練画像")
    right.bar(range(10), model.n_support_, color="#c0392b", label="サポートベクター")
    right.set(xticks=range(10), xlabel="数字", ylabel="枚数",
              title=f"判定に使う訓練画像: {model.n_support_.sum()} / {len(train_y)} 枚")
    right.set_ylim(0, 1.2 * train_counts.max())
    right.legend(fontsize=8, loc="upper center", ncols=2)
    fig.suptitle(f"SVM (RBF) C={c:g}, γ={gamma:g}: 正解率 {accuracy:.4f}、学習 {fit:.2f} 秒、予測 {predict:.2f} 秒", fontsize=11)
    report.figure()

    return result('svm', f'C={c:g}, gamma={gamma:g}', str(data['digest']), test_y, pred, fit, predict)
