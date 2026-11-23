"""Demo discovery metadata and parity for locally translated executable Cards."""

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


@pytest.mark.parametrize("demo", ["csharp-calibration", "signal-sweep"])
def test_localized_demos_share_executable_definitions(demo):
    japanese = ROOT / "demos" / "ja" / demo
    english = ROOT / "demos" / "en" / demo
    docs = [json.loads((root / "tsugumi.json").read_text()) for root in (japanese, english)]
    for doc in docs:
        del doc["name"]
    assert docs[0] == docs[1]
    for file in japanese.joinpath("definitions").rglob("*"):
        if not file.is_file() or file.name in {"README.md", "_comments.json"}:
            continue
        relative = file.relative_to(japanese)
        assert file.read_bytes() == (english / relative).read_bytes(), relative


@pytest.mark.parametrize("language", ["ja", "en"])
def test_demo_locks_and_input_definitions(language):
    for manifest in (ROOT / "demos" / language).rglob("tsugumi.json"):
        declarations = {entry["name"]: entry["url"] for entry in json.loads(manifest.read_text())["nixPath"]}
        lock = json.loads(manifest.with_name("tsugumi.lock").read_text())
        entries = lock["entries"]
        assert [entry["name"] for entry in entries] == sorted(declarations), manifest
        assert {entry["name"]: entry["url"] for entry in entries} == declarations, manifest
        assert all(entry["narHash"].startswith("sha256-") for entry in entries), manifest

        for definition in manifest.parent.glob("definitions/*/_card.json"):
            card = json.loads(definition.read_text())
            assert not {"sweeps", "retainArtifact", "artifactOutput"} & card.keys(), definition
            assert card["type"] in {"code", "draft", "trigger", "input", "merge", "collect", "subgraph"}, definition
            if card["type"] == "input":
                for name in ("default.nix", "params.nix", "_outputs.fbs"):
                    assert definition.with_name(name).is_file(), (definition, name)
