"""A multilayer perceptron: two fully connected hidden layers, trained on the CPU with a fixed seed."""
import os

# One thread: the numbers repeat and the times compare. Before the imports: the libraries read it as they load.
os.environ["OMP_NUM_THREADS"] = "1"

import time

import matplotlib.pyplot as plt
import numpy as np
import torch
from torch import nn


BATCH = 64


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
    hidden, epochs, seed = params.hidden, params.epochs, params.seed
    data = np.load(inputs.data.value.decode())
    test_y = data["test_labels"]
    train_x = torch.from_numpy(data["train_images"])
    train_y = torch.from_numpy(data["train_labels"]).long()
    test_x = torch.from_numpy(data["test_images"])

    torch.set_num_threads(1)
    torch.use_deterministic_algorithms(True)
    generator = torch.manual_seed(seed)
    model = nn.Sequential(nn.Linear(784, hidden), nn.ReLU(), nn.Linear(hidden, hidden), nn.ReLU(), nn.Linear(hidden, 10))
    optimizer = torch.optim.Adam(model.parameters(), lr=1e-3)
    loss_fn = nn.CrossEntropyLoss()

    losses = []
    start = time.process_time()
    for epoch in range(epochs):
        order = torch.randperm(len(train_x), generator=generator)
        total = 0.0
        for i in range(0, len(order), BATCH):
            batch = order[i:i + BATCH]
            optimizer.zero_grad()
            loss = loss_fn(model(train_x[batch]), train_y[batch])
            loss.backward()
            optimizer.step()
            total += loss.item() * len(batch)
        losses.append(total / len(order))
        report.log(f"epoch {epoch + 1}: train loss {losses[-1]:.4f}")
    fit = time.process_time() - start

    start = time.process_time()
    with torch.no_grad():
        pred = model(test_x).argmax(dim=1).numpy()
    predict = time.process_time() - start
    accuracy = np.mean(pred == test_y)
    report.log(f"mlp hidden={hidden} epochs={epochs}: accuracy {accuracy:.4f}, fit {fit:.3f} s, predict {predict:.3f} s")

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4.2), layout="constrained")
    confusions(left, test_y, pred)
    right.plot(range(1, epochs + 1), losses, marker="o", markersize=3, color="#c0392b")
    right.set(xlabel="Epoch", ylabel="Loss on the training data", ylim=(0, None), title="How training went")
    right.grid(alpha=0.3)
    fig.suptitle(f"MLP, width {hidden} × 2 layers, {epochs} epochs: accuracy {accuracy:.4f}, training {fit:.2f} s, prediction {predict:.3f} s", fontsize=11)
    report.figure()

    return result('mlp', f'hidden={hidden}, epochs={epochs}', str(data['digest']), test_y, pred, fit, predict)
