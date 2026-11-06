"""The graphs the 整列 tests lay out are the demos as they are now."""

from __future__ import annotations

import json

from arrange_cases import FIXTURE, all_cases


def test_the_arrange_fixture_is_what_the_demos_are():
    assert all_cases() == json.loads(FIXTURE.read_text()), "python3 tests/python/arrange_cases.py --write, then cd web && npm test -- --update-arrange"
