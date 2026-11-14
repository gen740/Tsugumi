"""Checks the MNIST files fixed by Nix (IDX format) and shows samples of each digit."""
import tsugumi
import struct

import matplotlib.pyplot as plt
import numpy as np

from card.dataset import DIRECTORY as directory, SOURCE as source  # written by default.nix
from tsugumi import Failure


def read_idx(path, magic, dims):
    """An IDX file: a big-endian magic number, the sizes, then one byte per value."""
    with open(path, "rb") as f:
        data = f.read()
    head = struct.unpack(f">{1 + dims}I", data[:4 * (1 + dims)])
    if head[0] != magic:
        raise Failure("output_invalid", f"{path}: magic number {head[0]}, expected {magic}")
    values = np.frombuffer(data, dtype=np.uint8, offset=4 * (1 + dims))
    return values.reshape(head[1:])


def main(inputs, report):
    train_images = read_idx(f'{directory}/train-images-idx3-ubyte', 2051, 3)
    train_labels = read_idx(f'{directory}/train-labels-idx1-ubyte', 2049, 1)
    test_images = read_idx(f'{directory}/t10k-images-idx3-ubyte', 2051, 3)
    test_labels = read_idx(f'{directory}/t10k-labels-idx1-ubyte', 2049, 1)
    if train_images.shape != (60000, 28, 28) or test_images.shape != (10000, 28, 28):
        raise Failure("output_invalid", f"unexpected shapes {train_images.shape}, {test_images.shape}")
    if len(train_labels) != 60000 or len(test_labels) != 10000:
        raise Failure("output_invalid", "the label files do not match the image files")
    report.log(str(f"source: {source}"))
    report.log(str(f"train: {train_images.shape}, test: {test_images.shape}, directory: {directory}"))

    per_digit = 20
    rows = [np.hstack(test_images[test_labels == d][:per_digit]) for d in range(10)]
    fig, ax = plt.subplots(figsize=(7.2, 4.4), layout="constrained")
    ax.imshow(np.vstack(rows), cmap="gray_r", vmin=0, vmax=255)
    ax.set_yticks(np.arange(10) * 28 + 14, labels=range(10))
    ax.set_xticks([])
    ax.tick_params(length=0)
    ax.set_title(f"MNIST 手書き数字: 訓練 {len(train_labels)} 枚、テスト {len(test_labels)} 枚\n"
                 f"テスト用から数字ごとに先頭 {per_digit} 枚。出典: {source}", fontsize=10)
    report.figure()
    directory_offset = directory
    source_offset = source
    _out_value = {}
    _out_value['directory'] = directory_offset
    _out_value['source'] = source_offset
    _out_value['train_count'] = len(train_labels)
    _out_value['test_count'] = len(test_labels)
    return {'dataset': _out_value}
