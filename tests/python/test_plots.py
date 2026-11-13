"""Plotly figures and Altair charts in the Card's display (tsugumi.plots),
against a stand-in for the worker's host module: fig.show() and chart.show()
as written anywhere send module displays, the size limit fails the call with
a reason, and nothing needs IPython or a browser."""

from __future__ import annotations

import json
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


def run(tmp_path, body: str) -> dict:
    (tmp_path / "_tsugumi_host.py").write_text(HOST)
    script = tmp_path / "case.py"
    script.write_text("import json\nimport _tsugumi_host as h\nimport tsugumi\n" + textwrap.dedent(body))
    env = {**os.environ, "PYTHONPATH": f"{tmp_path}{os.pathsep}{ROOT / 'sdk' / 'python'}",
           "MPLCONFIGDIR": str(tmp_path / "mpl"), "HOME": str(tmp_path)}
    out = subprocess.run([sys.executable, str(script)], env=env, capture_output=True, text=True, timeout=300)
    assert out.returncode == 0, out.stderr
    return json.loads(out.stdout.strip().splitlines()[-1])


def test_plotly_fig_show_sends_the_figure(tmp_path):
    r = run(tmp_path, '''
        import plotly.graph_objects as go
        ok = tsugumi._guard(lambda: go.Figure(go.Scatter(x=[1, 2], y=[3, 4]), layout_title_text="xy").show())
        name, data = h.modules[0]
        fig = json.loads(data)
        print(json.dumps({"ok": ok, "n": len(h.modules), "name": name, "type": fig["data"][0]["type"],
                          "title": fig["layout"]["title"]["text"]}))
    ''')
    assert r == {"ok": True, "n": 1, "name": "plotly", "type": "scatter", "title": "xy"}


def test_plotly_imported_before_the_sdk_is_set_up_too(tmp_path):
    r = run(tmp_path, '''
        # plotly.io already loaded: install() sets it up at once.
        import plotly.io as pio
        tsugumi._plots.install()
        print(json.dumps({"default": pio.renderers.default}))
    ''')
    assert r["default"] == "tsugumi"


def test_altair_chart_show_sends_the_spec_with_its_data(tmp_path):
    r = run(tmp_path, '''
        import altair as alt
        chart = alt.Chart(alt.Data(values=[{"x": 1, "y": 2}, {"x": 2, "y": 5}])).mark_line().encode(x="x:Q", y="y:Q")
        ok = tsugumi._guard(lambda: chart.properties(title="line").show())
        name, data = h.modules[0]
        spec = json.loads(data)
        print(json.dumps({"ok": ok, "name": name, "schema": spec["$schema"], "title": spec["title"],
                          "inline": bool(spec.get("datasets") or spec.get("data", {}).get("values"))}))
    ''')
    assert r["ok"] and r["name"] == "vega-lite" and "/vega-lite/v" in r["schema"] and r["title"] == "line" and r["inline"]


def test_display_figure_takes_plotly_and_altair(tmp_path):
    r = run(tmp_path, '''
        import altair as alt, plotly.graph_objects as go
        ctx = tsugumi.Context.__new__(tsugumi.Context)
        tsugumi._guard(lambda: ctx.display_figure(go.Figure(go.Bar(y=[1, 2]))))
        tsugumi._guard(lambda: ctx.display_figure(alt.Chart(alt.Data(values=[{"a": 1}])).mark_bar().encode(y="a:Q")))
        two = tsugumi._guard(lambda: ctx.display_figure(go.Figure(), go.Figure()))
        print(json.dumps({"names": [n for n, _ in h.modules], "two": two, "failures": h.failures}))
    ''')
    assert r["names"] == ["plotly", "vega-lite"] and r["two"] is False
    assert r["failures"][0][0] == "output_invalid"


def test_a_figure_over_the_limit_fails_with_a_reason(tmp_path):
    r = run(tmp_path, '''
        import plotly.graph_objects as go
        xs = list(range(150000))
        ok = tsugumi._guard(lambda: go.Figure(go.Scatter(x=xs, y=xs)).show())
        print(json.dumps({"ok": ok, "n": len(h.modules), "failures": h.failures}))
    ''')
    assert not r["ok"] and r["n"] == 0
    kind, message = r["failures"][0]
    assert kind == "output_invalid" and "at most 1 MiB" in message


def test_a_plot_is_not_replaced_by_a_matplotlib_figure_left_open(tmp_path):
    r = run(tmp_path, '''
        import matplotlib.pyplot as plt, plotly.graph_objects as go
        def body():
            plt.plot([1, 2])
            go.Figure(go.Bar(y=[1])).show()
        ok = tsugumi._guard(body)
        print(json.dumps({"ok": ok, "names": [n for n, _ in h.modules], "open": plt.get_fignums()}))
    ''')
    assert r == {"ok": True, "names": ["plotly"], "open": []}
