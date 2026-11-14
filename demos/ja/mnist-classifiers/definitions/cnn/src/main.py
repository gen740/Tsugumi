"""A small convolutional network: it sees the digit as an image, trained on the CPU with a fixed seed."""
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
    channels, epochs, seed = params.channels, params.epochs, params.seed
    data = np.load(inputs.data.value.decode())
    test_y = data["test_labels"]
    train_x = torch.from_numpy(data["train_images"]).reshape(-1, 1, 28, 28)
    train_y = torch.from_numpy(data["train_labels"]).long()
    test_x = torch.from_numpy(data["test_images"]).reshape(-1, 1, 28, 28)

    torch.set_num_threads(1)
    torch.use_deterministic_algorithms(True)
    generator = torch.manual_seed(seed)
    model = nn.Sequential(
        nn.Conv2d(1, channels, 3, padding=1), nn.ReLU(), nn.MaxPool2d(2),
        nn.Conv2d(channels, 2 * channels, 3, padding=1), nn.ReLU(), nn.MaxPool2d(2),
        nn.Flatten(), nn.Linear(2 * channels * 7 * 7, 64), nn.ReLU(), nn.Linear(64, 10),
    )
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
    report.log(f"cnn channels={channels} epochs={epochs}: accuracy {accuracy:.4f}, fit {fit:.3f} s, predict {predict:.3f} s")

    # What the first layer picks out of one test digit, one small image per filter.
    with torch.no_grad():
        maps = model[0](test_x[:1])[0].numpy()
    columns = int(np.ceil(np.sqrt(len(maps))))
    canvas = np.full((columns * 29 - 1, columns * 29 - 1), np.nan)
    for n, activation in enumerate(maps):
        row, column = n // columns, n % columns
        canvas[row * 29:row * 29 + 28, column * 29:column * 29 + 28] = activation
    limit = np.abs(maps).max()

    fig, (left, right) = plt.subplots(1, 2, figsize=(9, 4.2), layout="constrained")
    confusions(left, test_y, pred)
    right.imshow(canvas, cmap="RdBu_r", vmin=-limit, vmax=limit)
    right.set(xticks=[], yticks=[], title=f"1 層目の {len(maps)} 個のフィルターが、数字 {test_y[0]} から取り出したもの")
    fig.suptitle(f"CNN {channels} チャンネル、{epochs} エポック: 正解率 {accuracy:.4f}、学習 {fit:.2f} 秒、予測 {predict:.3f} 秒", fontsize=11)
    report.figure()

    return result('cnn', f'channels={channels}, epochs={epochs}', str(data['digest']), test_y, pred, fit, predict)
