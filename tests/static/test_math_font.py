"""The app uses only the default math font included in its bundle."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_the_app_draws_math_with_the_only_font_it_ships():
    sources = (ROOT / "ui/macos" / "Sources").rglob("*.swift")
    uses = [s.name for s in sources if re.search(r"MTFontManager|MathFont\b|\.font\s*=\s*MT", s.read_text())]
    assert not uses
