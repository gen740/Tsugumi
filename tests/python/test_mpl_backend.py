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
sent = []
cancel_after_frames = None
def display_image(media_type, data, alt): sent.append((media_type, bytes(data), alt))
def cancelled(): return cancel_after_frames is not None and len(sent) >= cancel_after_frames
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
import io
from PIL import Image
import _tsugumi_host as h
import tsugumi
import numpy as np
import tsugumi.mpl as backend
class Clock:
    now = 0.0
    def monotonic(self): return self.now
    def sleep(self, seconds): self.now += max(seconds, 0.000001)
clock = Clock()
backend.time = clock

def call(body):
    h.sent.clear()
    h.texts.clear()
    h.failures.clear()
    try:
        return tsugumi._guard(body)
    finally:
        h.cancel_after_frames = None

def image(frame):
    return Image.open(io.BytesIO(frame[1]))
'''


def run_case(tmp_path, body):
    (tmp_path / "_tsugumi_host.py").write_text(HOST)
    script = tmp_path / "case.py"
    script.write_text(PRELUDE + textwrap.dedent(body))
    env = {**os.environ, "PYTHONPATH": f"{tmp_path}{os.pathsep}{ROOT / 'sdk' / 'python'}",
           "MPLCONFIGDIR": str(tmp_path / "mpl"), "HOME": str(tmp_path)}
    env.pop("MPLBACKEND", None)
    out = subprocess.run([sys.executable, str(script)], env=env, capture_output=True, text=True, timeout=300)
    assert out.returncode == 0, out.stderr


def test_the_backend_is_chosen_and_show_sends_one_image(tmp_path):
    run_case(tmp_path, '''
        import matplotlib.pyplot as plt
        from matplotlib.animation import FuncAnimation

        def show():
            plt.plot([1, 2, 3], [1, 4, 9])
            plt.title("squares")
            tsugumi.Report().figure()

        assert call(show) and plt.get_backend() == "module://tsugumi.mpl"
        frame, = h.sent
        assert frame[0] == "image/png" and frame[2] == "squares" and not plt.get_fignums()

        def leave_figures_open():
            plt.figure()
            plt.plot([1, 2])
            plt.figure()
            plt.bar([1, 2], [3, 4])

        assert call(leave_figures_open) and not h.sent and not plt.get_fignums()

        def show_text():
            leave_figures_open()
            tsugumi.Report().text("the result")

        assert call(show_text) and not h.sent and h.texts == ["the result"] and not plt.get_fignums()

        def pause():
            fig, ax = plt.subplots()
            line, = ax.plot([], [])
            ax.set(xlim=(0, 60), ylim=(-1, 1))
            for i in range(12):
                x = np.arange(i * 3)
                line.set_data(x, np.sin(x / 5))
                tsugumi.Report().figure(wait=0.05)
            tsugumi.Report().figure(fig)

        assert call(pause) and not plt.get_fignums()
        assert len({frame[1] for frame in h.sent[:-1]}) > 1
        assert image(h.sent[-1]).size == tuple(2 * size for size in image(h.sent[0]).size)

        def animate():
            fig, ax = plt.subplots()
            line, = ax.plot([], [], "r-", lw=4)
            ax.set(xlim=(0, 20), ylim=(0, 20))
            animation = FuncAnimation(fig, lambda k: (line.set_data([0, k], [0, k]), line)[1:],
                                      frames=20, interval=20, blit=True, repeat=False)
            tsugumi.Report().figure()

        assert call(animate) and len({frame[1] for frame in h.sent}) > 1 and not plt.get_fignums()
        assert any(r > 200 and g < 80 and b < 80 for r, g, b in image(h.sent[-1]).convert("RGB").getdata())

        def endless():
            fig, ax = plt.subplots()
            line, = ax.plot([], [])
            animation = FuncAnimation(fig, lambda k: (line.set_data([0, k], [0, k]), line)[1:],
                                      interval=20, cache_frame_data=False)
            h.cancel_after_frames = 2
            tsugumi.Report().figure()
            assert h.cancelled()

        assert call(endless) and not plt.get_fignums()

        def fail():
            plt.plot([1])
            raise ValueError("boom")

        assert not call(fail) and not h.sent and not plt.get_fignums()
        assert h.failures[0][0] == "worker_failed"
    ''')


def test_a_card_that_picks_its_backend_keeps_it(tmp_path):
    run_case(tmp_path, '''
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        def body():
            fig = plt.figure()
            plt.plot([1, 2])
            plt.title("explicit")
            tsugumi.Report().figure(fig)
        assert call(body) and plt.get_backend().lower() == "agg"
        frame, = h.sent
        assert frame[2] == "explicit" and plt.get_fignums()
    ''')


def test_the_sdk_loads_no_native_library_before_the_card(tmp_path):
    """flatbuffers imports numpy. Were the SDK to import it first, the top of a
    Card's main.py would be too late for what numpy's libraries read as they load."""
    (tmp_path / "_tsugumi_host.py").write_text(HOST)
    env = {**os.environ, "PYTHONPATH": f"{tmp_path}{os.pathsep}{ROOT / 'sdk' / 'python'}"}
    code = 'import sys, tsugumi; print(sorted({"numpy", "flatbuffers"} & set(sys.modules)))'
    out = subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True, timeout=60)
    assert json.loads(out.stdout) == [], out.stdout + out.stderr
