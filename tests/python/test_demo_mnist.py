"""The MNIST classifier demo, run for real.

Checked against references outside the Cards: the class counts published for
MNIST, the digits of the kept subset (stratified, scaled to 0..1), and the kNN
accuracy recomputed with plain numpy from that subset. Then: the reported
accuracies clear floors taken from what the demo really gives, a rerun repeats
them exactly, and changing one branch's param moves only that branch.
"""

from __future__ import annotations

import io
import json
import os
import re

import numpy as np
import pytest

from conftest import ROOT, demo_environment
from projects import start_run, wait_run
from test_demos import connect, display_text, edit_card, set_param, stderr
from test_history import read_all
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_DEMO_RUNS") != "1",
                                reason="runs the demo (large Nix closures): TSUGUMI_TEST_DEMO_RUNS=1")

MODELS = ["knn", "logistic", "svm", "forest", "boosting", "mlp", "cnn"]

# Published per-class counts of MNIST (train, t10k).
TRAIN_COUNTS = [5923, 6742, 5958, 6131, 5842, 5421, 5918, 6265, 5851, 5949]
TEST_COUNTS = [980, 1135, 1032, 1010, 982, 892, 958, 1028, 974, 1009]

# Lower bounds a little under what the defaults gave on aarch64-darwin (see the PR).
FLOORS = {"knn": 0.94, "logistic": 0.91, "svm": 0.96, "forest": 0.94, "boosting": 0.935, "mlp": 0.95, "cnn": 0.955}


@pytest.fixture
def mnist(tmp_path):
    env = demo_environment(tmp_path, "mnist-classifiers")
    yield env
    env.cleanup()


def succeeded(c, run_id):
    st = wait_run(c, run_id, timeout=3000)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)


def leaderboard(c, run_id):
    """model -> accuracy, read from the table compare shows."""
    table = re.findall(r"^\| \d+ \| \*\*(\w+)\*\* \| ([\d.]+) \| ([\d.]+) \| ([\d.]+) \|", display_text(c, run_id, "compare"), re.M)
    assert [m for m, *_ in table] and sorted(m for m, *_ in table) == sorted(MODELS), table
    return {m: float(a) for m, a, _, _ in table}


def ran(c, run_id, card):
    try:
        return c.call("RunRecordRequest", runId=run_id, card=path(card)).record is not None
    except RpcFailure as e:
        assert "no record" in str(e), e  # what the run says of a Card it did not touch
        return False


def subset(c, run_id):
    art = next(a for a in c.call("ArtifactListRequest", runId=run_id).artifacts if text(a.output) == "subset")
    return np.load(io.BytesIO(read_all(c, "ArtifactReadRequest", 256 * 1024, artifactId=text(art.artifactId))))


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
    for m in MODELS:
        assert not stderr(c, first, m).strip().startswith("Traceback"), stderr(c, first, m)

    # The real MNIST: the table prepare shows carries the published counts.
    shown = display_text(c, first, "prepare")
    rows = re.findall(r"^\| (\d) \| (\d+) \| (\d+) \| (\d+) \| (\d+) \|$", shown, re.M)
    assert [int(r[1]) for r in rows] == TRAIN_COUNTS and [int(r[3]) for r in rows] == TEST_COUNTS

    # The subset is stratified (within one digit of the exact share) and scaled.
    data = subset(c, first)
    train_x, train_y = data["train_images"], data["train_labels"]
    test_x, test_y = data["test_images"], data["test_labels"]
    assert train_x.shape == (10000, 784) and test_x.shape == (2000, 784)
    assert train_x.min() == 0 and train_x.max() == 1
    assert np.all(np.abs(np.bincount(train_y, minlength=10) - 10000 * np.array(TRAIN_COUNTS) / 60000) <= 1)
    assert np.all(np.abs(np.bincount(test_y, minlength=10) - 2000 * np.array(TEST_COUNTS) / 10000) <= 1)

    scores = leaderboard(c, first)
    print("accuracies:", scores)

    # kNN recomputed here with numpy alone: the same accuracy as the Card's scikit-learn.
    reference = float(np.mean(plain_knn(train_x, train_y, test_x, k=3) == test_y))
    assert scores["knn"] == pytest.approx(reference, abs=1e-4)

    for m, floor in FLOORS.items():
        assert scores[m] > floor, (m, scores[m])
    assert scores["cnn"] > scores["knn"]
    assert scores["svm"] > scores["logistic"]  # a linear model cannot match the RBF kernel here

    # The views downstream of compare really drew.
    for card in ("load", "recall", "hard"):
        image = c.call("RunRecordRequest", runId=first, card=path(card)).record.display.value
        assert bytes(image.data[:4]) == b"\x89PNG" and len(image.data) > 10000
    module = c.call("RunRecordRequest", runId=first, card=path("tradeoff")).record.display.value
    figure = json.loads(text(module.dataJson))
    assert text(module.name) == "plotly"
    assert [sorted(t["text"]) for t in figure["data"] if t.get("mode") == "markers+text"] == [sorted(MODELS)] * 2

    # A second run gives the same numbers: seeds fixed, one thread.
    again = start_run(c, ["share"], concurrency=8)
    succeeded(c, again)
    assert leaderboard(c, again) == scores

    # One branch's param, one Card run: its accuracy moves, the others do not run.
    edit_card(mnist, c, "forest", lambda body: set_param(body, "max_depth", 3))
    alone = start_run(c, ["forest"], propagate=False)
    succeeded(c, alone)
    assert ran(c, alone, "forest")
    for card in [m for m in MODELS if m != "forest"] + ["compare", "prepare"]:
        assert not ran(c, alone, card), card
    log = text(c.call("LogRangeRequest", runId=alone, card=path("forest"), stream=1, limitBytes=4096).text)
    shown = re.search(r"accuracy ([\d.]+)", log)
    print("forest at depth 3:", shown.group(1))
    assert float(shown.group(1)) < scores["forest"] - 0.05

    # From that branch onward: compare takes the new forest and the saved values
    # of the others, none of which runs again.
    onward = start_run(c, ["forest"])
    succeeded(c, onward)
    assert ran(c, onward, "forest") and ran(c, onward, "compare") and ran(c, onward, "tradeoff")
    for card in [m for m in MODELS if m != "forest"] + ["share", "prepare"]:
        assert not ran(c, onward, card), card
    moved = leaderboard(c, onward)
    assert moved["forest"] == pytest.approx(float(shown.group(1)), abs=1e-4)
    assert {m: a for m, a in moved.items() if m != "forest"} == {m: a for m, a in scores.items() if m != "forest"}


def test_the_finished_cards_fit_the_draft_demo(tmp_path):
    """What the helpers are asked for can be done: the finished classifiers,
    saved over the Drafts of mnist-classifiers-draft as a helper saves, run on
    its data and clear the floors written in the Drafts, and compare reads them."""
    env = demo_environment(tmp_path, "mnist-classifiers-draft")
    try:
        c = connect(env)
        finished = ROOT / "demos" / "mnist-classifiers" / "definitions"
        changes = []
        for card in MODELS + ["compare"]:
            body = json.loads((finished / card / "_card.json").read_text())
            if os.environ.get("TSUGUMI_TEST_CONTAINER") != "1":
                body["sandbox"] = False  # as demo_environment does on a host
            changes.append(change("PutDefinition", id=card, cardJson=json.dumps(body), keepMeta=True))
            for f in ["default.nix", "_outputs.fbs", "src/main.py"]:
                changes.append(change("PutSource", path=f"definitions/{card}/{f}", bytes=list((finished / card / f).read_bytes())))
        c.call("ProjectSaveRequest", baseRev=c.call("ProjectInspectRequest").rev, opId=new_id(), origin="test", changes=changes)
        run_id = start_run(c, ["start"])
        succeeded(c, run_id)
        scores = leaderboard(c, run_id)
        drafts = ROOT / "demos" / "mnist-classifiers-draft" / "definitions"
        for card in MODELS:
            content = json.loads((drafts / card / "_meta.json").read_text())["content"]
            floor = float(re.search(r"正解率が ([\d.]+) 以上", content).group(1))
            assert scores[card] >= floor, (card, scores[card], floor)
    finally:
        env.cleanup()
