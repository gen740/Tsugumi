"""Checks the MNIST files fixed by Nix (IDX format) and shows samples of each digit."""
import struct

import numpy as np

from dataset import DIRECTORY as directory, SOURCE as source  # written by default.nix
from card import Artifact, Input, Output
from mnist.Dataset import DatasetT
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


def run(input: Input) -> Output:
    train_images = read_idx(f'{directory}/train-images-idx3-ubyte', 2051, 3)
    train_labels = read_idx(f'{directory}/train-labels-idx1-ubyte', 2049, 1)
    test_images = read_idx(f'{directory}/t10k-images-idx3-ubyte', 2051, 3)
    test_labels = read_idx(f'{directory}/t10k-labels-idx1-ubyte', 2049, 1)
    if train_images.shape != (60000, 28, 28) or test_images.shape != (10000, 28, 28):
        raise Failure("output_invalid", f"unexpected shapes {train_images.shape}, {test_images.shape}")
    if len(train_labels) != 60000 or len(test_labels) != 10000:
        raise Failure("output_invalid", "the label files do not match the image files")
    print(f"source: {source}")
    print(f"train: {train_images.shape}, test: {test_images.shape}, directory: {directory}")

    # TODO(open): Port the demo display.
    return Output(dataset=DatasetT(
        directory=Artifact.upload(directory), source=source,
        trainCount=len(train_labels), testCount=len(test_labels)
    ))
