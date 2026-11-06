"""The matplotlib backend of the Python SDK (tsugumi.mpl) against a stand-in for
the worker's host module: what a Card draws reaches its display, live while it
runs, and no figure is left in the worker afterwards."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

pytest.importorskip("matplotlib")

ROOT = Path(__file__).resolve().parents[2]

HOST = '''
import time
sent = []
cancel_at = None
def display_image(media_type, data, alt): sent.append((time.monotonic(), media_type, bytes(data), alt))
def cancelled(): return cancel_at is not None and time.monotonic() >= cancel_at
def remaining_ms(): return 60000
def inputs(): return []
def info(): return {}
def environ(): return list(__import__("os").environb.items())
failures = []
def fail(kind, message): failures.append((kind, message))
texts = []
def display_text(text): texts.append(text)
'''

PRELUDE = '''
import json, sys, time
import _tsugumi_host as h
import tsugumi
import numpy as np

def call(body):
    h.sent.clear()
    ok = tsugumi._guard(body)
    import matplotlib.pyplot as plt
    return {"ok": ok, "frames": len(h.sent), "types": sorted({s[1] for s in h.sent}),
            "alts": [s[3] for s in h.sent], "open": plt.get_fignums(), "failures": h.failures,
            "sizes": [list(__import__("PIL.Image").Image.open(__import__("io").BytesIO(s[2])).size) for s in h.sent]}
'''


@pytest.fixture(scope="module")
def mpl_home(tmp_path_factory):
    """One matplotlib configuration folder (and font cache) for the module."""
    return tmp_path_factory.mktemp("mpl")


@pytest.fixture
def run(tmp_path, mpl_home):
    return lambda body: run_case(tmp_path, mpl_home, body)


def run_case(tmp_path, mpl_home, body: str) -> dict:
    """Runs ``body`` (which prints one JSON line) with the SDK and the stand-in host."""
    (tmp_path / "_tsugumi_host.py").write_text(HOST)
    script = tmp_path / "case.py"
    script.write_text(PRELUDE + textwrap.dedent(body))
    env = {**os.environ, "PYTHONPATH": f"{tmp_path}{os.pathsep}{ROOT / 'sdk' / 'python'}",
           "MPLCONFIGDIR": str(mpl_home), "HOME": str(tmp_path)}
    env.pop("MPLBACKEND", None)
    out = subprocess.run([sys.executable, str(script)], env=env, capture_output=True, text=True, timeout=300)
    assert out.returncode == 0, out.stderr
    return json.loads(out.stdout.strip().splitlines()[-1])


def test_the_backend_is_chosen_and_show_sends_one_image(run):
    r = run('''
        import matplotlib.pyplot as plt
        def body():
            plt.plot([1, 2, 3], [1, 4, 9]); plt.title("squares"); plt.show()
        res = call(body); res["backend"] = plt.get_backend(); print(json.dumps(res))
    ''')
    assert r["backend"] == "module://tsugumi.mpl"
    assert r["ok"] and r["frames"] == 1 and r["types"] == ["image/png"] and r["alts"] == ["squares"]
    assert r["open"] == []  # show() closes the figures, as closing the window would


def test_figures_left_open_are_shown_and_closed(run):
    r = run('''
        import matplotlib.pyplot as plt
        def body():
            plt.figure(); plt.plot([1, 2]); plt.title("a")
            plt.figure(); plt.bar([1, 2], [3, 4]); plt.title("b")
        print(json.dumps(call(body)))
    ''')
    # Both figures in one image (the Card shows one display), one above the other.
    assert r["ok"] and r["frames"] == 1 and r["alts"] == ["a; b"] and r["open"] == []
    w, h = r["sizes"][0]
    assert h > w


def test_pause_sends_live_frames_and_a_sharp_last_one(run):
    r = run('''
        import matplotlib.pyplot as plt
        def body():
            fig, ax = plt.subplots(); line, = ax.plot([], []); ax.set_xlim(0, 60); ax.set_ylim(-1, 1)
            for i in range(20):
                line.set_data(np.arange(i * 3), np.sin(np.arange(i * 3) / 5)); plt.pause(0.05)
        print(json.dumps(call(body)))
    ''')
    assert r["ok"] and r["frames"] >= 5 and r["open"] == []
    live, last = r["sizes"][0], r["sizes"][-1]
    assert last[0] == 2 * live[0] and last[1] == 2 * live[1]  # live frames at 1x, the last at 2x


def test_animations_play_while_show_waits(run):
    r = run('''
        import matplotlib.pyplot as plt
        from matplotlib.animation import FuncAnimation
        def body():
            fig, ax = plt.subplots(); ln, = ax.plot([], [], "r-", lw=4); ax.set_xlim(0, 20); ax.set_ylim(0, 20)
            anim = FuncAnimation(fig, lambda k: (ln.set_data([0, k], [0, k]), ln)[1:], frames=20,
                                 interval=20, blit=True, repeat=False)
            plt.show()
        res = call(body)
        from PIL import Image
        import io
        im = Image.open(io.BytesIO(h.sent[-1][2])).convert("RGB")
        res["red"] = sum(1 for p in im.getdata() if p[0] > 200 and p[1] < 80 and p[2] < 80)
        print(json.dumps(res))
    ''')
    assert r["ok"] and r["frames"] >= 3 and r["open"] == []
    assert r["red"] > 1000  # the animated line is in the image (no blitting)


def test_an_endless_animation_ends_when_the_run_is_stopped(run):
    r = run('''
        import matplotlib.pyplot as plt
        from matplotlib.animation import FuncAnimation
        def body():
            fig, ax = plt.subplots(); ln, = ax.plot([], [])
            anim = FuncAnimation(fig, lambda k: (ln.set_data([0, k], [0, k]), ln)[1:], interval=20,
                                 cache_frame_data=False)
            h.cancel_at = time.monotonic() + 0.5
            plt.show()
        t0 = time.monotonic(); res = call(body); res["took"] = time.monotonic() - t0
        print(json.dumps(res))
    ''')
    assert r["ok"] and r["frames"] >= 2 and r["open"] == [] and r["took"] < 5


def test_a_failing_call_leaves_no_figure(run):
    r = run('''
        import matplotlib.pyplot as plt
        def body():
            plt.plot([1]); raise ValueError("boom")
        print(json.dumps(call(body)))
    ''')
    assert not r["ok"] and r["frames"] == 0 and r["open"] == []
    assert r["failures"] == [["worker_failed", "ValueError: boom"]]


def test_a_card_that_picks_its_backend_keeps_it(run):
    r = run('''
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        def body():
            fig = plt.figure(); plt.plot([1, 2]); plt.title("explicit")
            tsugumi.Context.__new__(tsugumi.Context).display_figure(fig)
        res = call(body); res["backend"] = plt.get_backend(); print(json.dumps(res))
    ''')
    # Agg: nothing is sent by itself, figures stay as the Card left them; display_figure still works.
    assert r["backend"].lower() == "agg"
    assert r["frames"] == 1 and r["alts"] == ["explicit"] and r["open"] == [1]


def test_a_display_of_the_card_is_not_replaced_by_a_figure_left_open(run):
    r = run('''
        import matplotlib.pyplot as plt
        def body():
            plt.plot([1, 2]); plt.title("forgotten")
            tsugumi.Context.__new__(tsugumi.Context).display_text("the result")
        res = call(body); res["texts"] = h.texts; print(json.dumps(res))
    ''')
    assert r["ok"] and r["frames"] == 0 and r["texts"] == ["the result"] and r["open"] == []


def test_the_sdk_loads_no_native_library_before_the_card(tmp_path):
    """flatbuffers imports numpy. Were the SDK to import it first, the top of a
    Card's main.py would be too late for what numpy's libraries read as they load."""
    (tmp_path / "_tsugumi_host.py").write_text(HOST)
    env = {**os.environ, "PYTHONPATH": f"{tmp_path}{os.pathsep}{ROOT / 'sdk' / 'python'}"}
    code = 'import sys, tsugumi; print(sorted({"numpy", "flatbuffers"} & set(sys.modules)))'
    out = subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True, timeout=60)
    assert out.stdout.strip() == "[]", out.stdout + out.stderr
