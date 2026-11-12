"""The Collatz demo in all four languages (C++, Python, Julia and R), run for real.

test_demos.py runs the loop of C++ and Python in CI (a copy of the demo without the other two). Here the whole demo
runs, with all four languages using resident workers: 27 gives the same
112 values in every language, and a value R cannot hold exactly fails its Card while Julia keeps the exact sequence.
"""

from __future__ import annotations

import os

import pytest

from conftest import demo_environment
from projects import start_run, wait_run
from test_demos import collatz_reference, connect, display_text, edit_card, set_param, stderr
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

pytestmark = pytest.mark.skipif(os.environ.get("TSUGUMI_TEST_DEMO_RUNS") != "1",
                                reason="runs the demo (large Nix closures): TSUGUMI_TEST_DEMO_RUNS=1")

LANGUAGES = ("collatz-cpp", "collatz-py", "collatz-jl", "collatz-r")


@pytest.fixture
def demo(tmp_path):
    env = demo_environment(tmp_path, "collatz")
    yield env
    env.cleanup()


def test_collatz_27_in_four_languages(demo):
    c = connect(demo)
    rid = start_run(c, ["start"])
    st = wait_run(c, rid, timeout=1800)
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    expected = collatz_reference(27)
    assert len(expected) == 112
    shown = c.call("RunRecordRequest", runId=rid, card=path("compare")).record.display.value
    assert shown.markdown and text(shown.text).startswith("**C++, Python, Julia and R agree**: 112 values, $a_0 = 27$")
    assert "after $111$ steps" in text(shown.text) and "9232" in text(shown.text)
    # Every language collected the sequence it computed.
    for lang in LANGUAGES:
        assert display_text(c, rid, lang, "collect") == "112 values: " + " ".join(map(str, expected)), lang


def test_collatz_r_fails_rather_than_round(demo):
    """R holds an integer exactly only below 2^53: past it the R Card in Card fails and says so, and Julia, which
    computes in UInt64, still returns the exact sequence. (4^28 - 1) / 3 is odd and not a double (2^53 < n), and
    3n + 1 = 2^56 sends it to 1 in 56 halvings."""
    c = connect(demo)
    n = (4 ** 28 - 1) // 3
    assert n > 2 ** 53 and float(n) != n
    edit_card(demo, c, "initial", lambda b: set_param(b, "value", 0), raw=lambda s: s.replace('"data": 0', f'"data": {n}'))
    rid = start_run(c, ["start"])
    st = wait_run(c, rid, timeout=1800)
    assert st.state == RunState.failed
    assert "2^53" in stderr(c, rid, "collatz-r", "check")
    # The failed run waits for the branches that go on: Julia has the exact sequence.
    expected = [n] + [2 ** k for k in range(56, -1, -1)]
    assert display_text(c, rid, "collatz-jl", "collect") == f"{len(expected)} values: " + " ".join(map(str, expected))
