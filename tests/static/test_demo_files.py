"""The bundled demos' files are consistent (no daemon, no Nix): one set per
UI language, a summary on every Card definition, the Drafts well formed.
Their behavior is in test_demos.py and the other test_demo_*.py files."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]

DEMOS = json.loads((ROOT / "demos" / "ja" / "index.json").read_text())["demos"]


def test_every_implemented_demo_has_a_field():
    """The guide's first questions list the demos by field (demos/index.json
    "field"): each implemented demo is in one; a Draft is listed apart."""
    bad = [d["id"] for d in DEMOS if bool(d.get("field", "").strip()) == d["id"].endswith("-draft")]
    assert not bad, bad


def test_the_japanese_and_english_demos_are_the_same_demos():
    """demos/ja and demos/en are each a set of their own, one per UI language:
    the same demos, made of the same files (their text in one language)."""
    files = {lang: sorted(str(p.relative_to(ROOT / "demos" / lang)) for p in (ROOT / "demos" / lang).rglob("*") if p.is_file())
             for lang in ("ja", "en")}
    assert files["ja"] == files["en"]
    ids = {lang: [d["id"] for d in json.loads((ROOT / "demos" / lang / "index.json").read_text())["demos"]] for lang in ("ja", "en")}
    assert ids["ja"] == ids["en"]


@pytest.mark.parametrize("language", ["ja", "en"])
def test_every_bundled_card_says_what_it_does(language):
    """Every definition of every demo, inside Cards in Card too, has a summary
    in its _meta.json, which is where the Canvas and Card settings read it."""
    definitions = sorted(p for p in (ROOT / "demos" / language).rglob("definitions/*") if p.is_dir())
    assert len(definitions) > 200
    bad = []
    for d in definitions:
        where = d.relative_to(ROOT / "demos" / language)
        meta = d / "_meta.json"
        summary = json.loads(meta.read_text()).get("description", "").strip() if meta.exists() else ""
        if not summary or summary == d.name:
            bad.append(f"{where}: no summary")
        card = d / "_card.json"
        if card.exists() and "description" in json.loads(card.read_text()):
            bad.append(f"{where}: description in _card.json")
    assert not bad, bad


def test_the_mnist_draft_is_work_for_helpers_that_meets_again():
    """Seven Drafts helpers can write at the same time (no wire between them,
    no more than delegate takes), with nothing of an answer in them, and one
    output type written out in each so that compare can read what they wrote."""
    demo = ROOT / "demos" / "ja" / "mnist-classifiers-draft"
    doc = json.loads((demo / "tsugumi.json").read_text())
    classifiers = {"knn", "logistic", "svm", "forest", "boosting", "mlp", "cnn"}
    template = {c["id"]: json.loads((demo / "definitions" / c["id"] / "_card.json").read_text()).get("template")
                for c in doc["canvas"]["cards"]}
    assert {card for card, t in template.items() if t == "draft"} == classifiers | {"compare"}
    assert (demo / "definitions" / "load" / "src" / "main.py").is_file()
    assert (demo / "definitions" / "prepare" / "src" / "main.py").is_file()
    wires = {(w["from"]["card"], w["to"]["card"]) for w in doc["canvas"]["connections"]}
    assert not {w for w in wires if w[0] in classifiers and w[1] in classifiers}
    assert {("share", m) for m in classifiers} | {(m, "compare") for m in classifiers} <= wires
    limit = json.loads((ROOT / "agent" / "Shared" / "agent.json").read_text())["delegateTool"]
    assert len(classifiers) <= limit["inputSchema"]["properties"]["tasks"]["maxItems"]
    # The type of the finished demo, the one compare reads.
    finished = (ROOT / "demos" / "ja" / "mnist-classifiers" / "definitions" / "knn" / "_outputs.fbs").read_text()
    contract = " ".join(finished.split())
    for card in classifiers | {"compare"}:
        folder = demo / "definitions" / card
        assert sorted(f.name for f in folder.iterdir()) == ["_card.json", "_meta.json"], card
        content = json.loads((folder / "_meta.json").read_text())["content"]
        assert "assert" in content, card
        if card != "compare":
            assert contract in " ".join(content.split()), card
