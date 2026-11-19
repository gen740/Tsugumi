"""Demo discovery metadata and parity for locally translated executable Cards."""

from __future__ import annotations

import json
import re
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
    localized_report = Path("definitions/summary/default.nix")
    for file in japanese.joinpath("definitions").rglob("*"):
        if not file.is_file() or file.name == "_meta.json":
            continue
        relative = file.relative_to(japanese)
        other = english / relative
        if demo == "signal-sweep" and relative == localized_report:
            # Ignore the translated prose, preserving the jq expressions the
            # report interpolates as well as the rest of the Card's program.
            def normalize(source):
                def report(match):
                    values = sorted(re.findall(r"\\\(([^)]+)\)", match[1]))
                    assert values == ["$thresholds", ".out.comparisons", ".out.detections"]
                    return '{text: "localized"}'
                normalized, count = re.subn(r'{text: "([^"]*)"}', report, source)
                assert count == 1
                return normalized

            assert normalize(file.read_text()) == normalize(other.read_text())
        else:
            assert file.read_bytes() == other.read_bytes(), relative
