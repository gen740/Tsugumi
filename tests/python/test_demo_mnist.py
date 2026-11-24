"""The MNIST classifier demo, run for real.

Checked against references outside the Cards: the class counts published for
MNIST, the digits of the kept subset (stratified, scaled to 0..1), and the kNN
accuracy recomputed with plain NumPy from that subset.
"""

from __future__ import annotations

# TODO(Card ABI): the bundled demos are still on the previous Card ABI and are ported once the
# recording of displays is designed (docs/Demos.md, "TODO: port the demos to
# the current Card ABI"). Until then these demo runs are skipped, not ported.
import pytest

pytest.skip("TODO(Card ABI): demos are postponed until display recording is designed; "
            "see docs/Demos.md", allow_module_level=True)

import io
import os
import re

import numpy as np
import pytest

from conftest import demo_environment
from projects import start_run, wait_run
from test_demos import connect, display_text
from harness.observations import read_pages
from tsugumi_client import text
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_DEMO_RUNS") != "1",
                                reason="runs the demo (large Nix closures): TSUGUMI_TEST_DEMO_RUNS=1")

MODELS = ["knn", "logistic", "svm", "forest", "boosting", "mlp", "cnn"]

# Published per-class counts of MNIST (train, t10k).
TRAIN_COUNTS = [5923, 6742, 5958, 6131, 5842, 5421, 5918, 6265, 5851, 5949]
TEST_COUNTS = [980, 1135, 1032, 1010, 982, 892, 958, 1028, 974, 1009]

@pytest.fixture
def mnist(tmp_path):
    env = demo_environment(tmp_path, "mnist-classifiers")
    yield env
    env.cleanup()


def succeeded(c, run_id):
    st = wait_run(c, run_id)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)


def leaderboard(c, run_id):
    """model -> accuracy, read from the table compare shows."""
    table = re.findall(r"^\| \d+ \| \*\*(\w+)\*\* \| ([\d.]+) \| ([\d.]+) \| ([\d.]+) \|", display_text(c, run_id, "compare"), re.M)
    assert [m for m, *_ in table] and sorted(m for m, *_ in table) == sorted(MODELS), table
    return {m: float(a) for m, a, _, _ in table}


def subset(c, run_id):
    art = next(a for a in c.call("ArtifactListRequest", runId=run_id).artifacts if text(a.output) == "subset")
    return np.load(io.BytesIO(read_pages(c, "ArtifactReadRequest", page_bytes=256 * 1024, artifactId=text(art.artifactId))))


def plain_knn(train_x, train_y, test_x, k):
    """Majority of the k nearest by Euclidean distance; ties go to the smallest digit."""
    pred = np.empty(len(test_x), dtype=np.uint8)
    for i in range(0, len(test_x), 200):
        batch = test_x[i:i + 200]
        d = (batch**2).sum(1)[:, None] - 2 * batch @ train_x.T + (train_x**2).sum(1)
        nearest = train_y[np.argsort(d, axis=1, kind="stable")[:, :k]]
        pred[i:i + 200] = [np.bincount(v, minlength=10).argmax() for v in nearest]
    return pred


def test_mnist_classifiers(mnist):
    c = connect(mnist)
    first = start_run(c, ["start"], concurrency=8)
    succeeded(c, first)

    # The real MNIST: the table prepare shows carries the published counts.
    shown = display_text(c, first, "prepare")
    rows = re.findall(r"^\| (\d) \| (\d+) \| (\d+) \| (\d+) \| (\d+) \|$", shown, re.M)
    assert [int(r[1]) for r in rows] == TRAIN_COUNTS and [int(r[3]) for r in rows] == TEST_COUNTS

    # The subset is stratified (within one digit of the exact share) and scaled.
    data = subset(c, first)
    train_x, train_y = data["train_images"], data["train_labels"]
    test_x, test_y = data["test_images"], data["test_labels"]
    assert train_x.min() == 0 and train_x.max() == 1
    assert np.all(np.abs(np.bincount(train_y, minlength=10) - len(train_y) * np.array(TRAIN_COUNTS) / 60000) <= 1)
    assert np.all(np.abs(np.bincount(test_y, minlength=10) - len(test_y) * np.array(TEST_COUNTS) / 10000) <= 1)

    scores = leaderboard(c, first)

    # kNN recomputed here with numpy alone: the same accuracy as the Card's scikit-learn.
    reference = float(np.mean(plain_knn(train_x, train_y, test_x, k=3) == test_y))
    assert scores["knn"] == pytest.approx(reference, abs=1e-4)
