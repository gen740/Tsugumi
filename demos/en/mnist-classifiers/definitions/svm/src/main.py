"""A support vector machine with an RBF kernel: decides by similarity to a few training digits."""
import os

# One thread: the numbers repeat and the times compare. Before the imports: the libraries read it as they load.
os.environ["OMP_NUM_THREADS"] = "1"

import time

import matplotlib.pyplot as plt
import numpy as np
from sklearn.svm import SVC

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
    c, gamma = ctx.params().C(), ctx.params().Gamma()
    data = np.load(ctx.input("data", SubsetFile).Value().decode())
    train_x, train_y, test_x, test_y = data["train_images"], data["train_labels"], data["test_images"], data["test_labels"]

    model = SVC(C=c, gamma=gamma, kernel="rbf")
    start = time.process_time()
    model.fit(train_x, train_y)
    fit = time.process_time() - start
    start = time.process_time()
    pred = model.predict(test_x)
    predict = time.process_time() - start
    accuracy = np.mean(pred == test_y)
    print(f"svm C={c} gamma={gamma}: accuracy {accuracy:.4f}, {model.n_support_.sum()} support vectors, "
          f"fit {fit:.3f} s, predict {predict:.3f} s")

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4.2), layout="constrained")
    confusions(left, test_y, pred)
    train_counts = np.bincount(train_y, minlength=10)
    right.bar(range(10), train_counts, color="#d9d9d9", label="Training images")
    right.bar(range(10), model.n_support_, color="#c0392b", label="Support vectors")
    right.set(xticks=range(10), xlabel="Digit", ylabel="Images",
              title=f"Training images used in decisions: {model.n_support_.sum()} / {len(train_y)}")
    right.set_ylim(0, 1.2 * train_counts.max())
    right.legend(fontsize=8, loc="upper center", ncols=2)
    fig.suptitle(f"SVM (RBF) C={c:g}, γ={gamma:g}: accuracy {accuracy:.4f}, training {fit:.2f} s, prediction {predict:.2f} s", fontsize=11)
    plt.show()

    send(ctx, "svm", f"C={c:g}, gamma={gamma:g}", str(data["digest"]), test_y, pred, fit, predict)
