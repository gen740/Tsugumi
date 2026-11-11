"""Draws a stratified subset, scales the pixels to 0..1 and writes it for every classifier."""
import hashlib
import os

import numpy as np

from inputs.dataset.mnist.Dataset import Dataset
import outputs.subset.mnist.SubsetFile as out
from tsugumi import Failure


def read(path, header, shape=(-1,)):
    with open(path, "rb") as f:
        return np.frombuffer(f.read(), dtype=np.uint8, offset=header).reshape(shape)


def stratified(labels, size, rng):
    """`size` indices keeping the share of each digit (largest remainders round it)."""
    counts = np.bincount(labels, minlength=10)
    quota = size * counts / counts.sum()
    take = np.floor(quota).astype(int)
    take[np.argsort(take - quota, kind="stable")[: size - take.sum()]] += 1
    chosen = [rng.permutation(np.flatnonzero(labels == d))[: take[d]] for d in range(10)]
    return rng.permutation(np.concatenate(chosen))


def main(ctx):
    dataset = ctx.input("dataset", Dataset)
    directory = dataset.Directory().decode()
    params = ctx.params()
    seed, train_size, test_size = params.Seed(), params.TrainSize(), params.TestSize()
    if not 10 <= train_size <= dataset.TrainCount() or not 10 <= test_size <= dataset.TestCount():
        raise Failure("output_invalid", f"train_size must be 10..{dataset.TrainCount()}, "
                                        f"test_size 10..{dataset.TestCount()}")

    all_train_x = read(f"{directory}/train-images-idx3-ubyte", 16, (-1, 784))
    all_train_y = read(f"{directory}/train-labels-idx1-ubyte", 8)
    all_test_x = read(f"{directory}/t10k-images-idx3-ubyte", 16, (-1, 784))
    all_test_y = read(f"{directory}/t10k-labels-idx1-ubyte", 8)

    rng = np.random.default_rng(seed)
    train = stratified(all_train_y, train_size, rng)
    test = stratified(all_test_y, test_size, rng)
    train_x = all_train_x[train].astype(np.float32) / 255
    test_x = all_test_x[test].astype(np.float32) / 255
    train_y, test_y = all_train_y[train], all_test_y[test]
    digest = hashlib.sha256(test_x.tobytes() + test_y.tobytes()).hexdigest()[:16]

    path = os.path.join(ctx.output_directory, "subset.npz")
    np.savez_compressed(path, train_images=train_x, train_labels=train_y,
                        test_images=test_x, test_labels=test_y, digest=digest)
    print(f"wrote {path}: {os.path.getsize(path)} bytes, test digest {digest}")

    rows = [f"| {d} | {np.sum(all_train_y == d)} | {np.sum(train_y == d)} | "
            f"{np.sum(all_test_y == d)} | {np.sum(test_y == d)} |" for d in range(10)]
    ctx.display_markdown(
        f"**Stratified sample** seed {seed}: {train_size} training, {test_size} test images (keeping the share of each digit). "
        f"Pixels are 0–1, 784 dimensions.\n\n"
        "| Digit | Training (all) | Training (used) | Test (all) | Test (used) |\n| ---: | ---: | ---: | ---: | ---: |\n"
        + "\n".join(rows)
    )

    b = ctx.builder()
    value = b.CreateString(path)
    out.Start(b)
    out.AddValue(b, value)
    ctx.output("subset", b, out.End(b))
