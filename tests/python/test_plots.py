"""Python SDK Plotly and Altair displays against the worker host interface."""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

pytest.importorskip("plotly")
pytest.importorskip("altair")

ROOT = Path(__file__).resolve().parents[2]

HOST = '''
modules = []
failures = []
def display_module(name, data, blobs): modules.append((name, data))
def display_image(media_type, data, alt): modules.append(("image", alt))
def fail(kind, message): failures.append((kind, message))
def inputs(): return []
def info(): return {}
def environ(): return list(__import__("os").environb.items())
def cancelled(): return False
def remaining_ms(): return 60000
'''


def test_report_figure_sends_plotly(tmp_path):
    (tmp_path / "_tsugumi_host.py").write_text(HOST)
    script = tmp_path / "case.py"
    script.write_text(textwrap.dedent('''
        import json
        import _tsugumi_host as h
        import tsugumi
        import altair as alt
        import plotly.graph_objects as go
        import plotly.io

        assert not h.modules
        report = tsugumi.Report()
        plot = go.Figure(go.Scatter(x=[1, 2], y=[3, 4], mode="markers"), layout_title_text="xy")
        chart = alt.Chart(alt.Data(values=[{"x": 1, "y": 2}, {"x": 2, "y": 5}])).mark_line().encode(x="x:Q", y="y:Q")

        assert tsugumi._guard(lambda: report.figure(plot))
        name, data = h.modules.pop()
        figure = json.loads(data)
        assert name == "plotly" and figure["data"][0]["type"] == "scatter"
        assert figure["layout"]["title"]["text"] == "xy"

        assert tsugumi._guard(lambda: report.figure(chart.properties(title="line")))
        name, data = h.modules.pop()
        spec = json.loads(data)
        assert name == "vega-lite" and "/vega-lite/v" in spec["$schema"] and spec["title"] == "line"
        assert spec.get("datasets") or spec.get("data", {}).get("values")

        assert not tsugumi._guard(lambda: report.figure(plot, plot))
        assert h.failures.pop()[0] == "output_invalid" and not h.modules

        xs = list(range(150000))
        assert not tsugumi._guard(lambda: report.figure(go.Figure(go.Scatter(x=xs, y=xs))))
        assert h.failures.pop()[0] == "output_invalid" and not h.modules

        import matplotlib.pyplot as plt
        def leave_figure_open():
            plt.plot([1, 2])
            report.figure(plot)

        assert tsugumi._guard(leave_figure_open)
        assert [name for name, _ in h.modules] == ["plotly"] and not plt.get_fignums()
    '''))
    env = {**os.environ, "PYTHONPATH": f"{tmp_path}{os.pathsep}{ROOT / 'sdk' / 'python'}",
           "MPLCONFIGDIR": str(tmp_path / "mpl"), "HOME": str(tmp_path)}
    env.pop("MPLBACKEND", None)
    result = subprocess.run([sys.executable, str(script)], env=env, capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, result.stdout + result.stderr
