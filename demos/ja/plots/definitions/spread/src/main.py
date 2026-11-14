"""Plain Altair, as written for a notebook: report.figure(chart) puts the interactive
chart in the Card's display (its data inline; pan and zoom in the expanded
view, a tooltip on each point)."""
import altair as alt
import numpy as np


def main(inputs, report):
    w = inputs.walks
    paths = w.positions.reshape(w.walkers, w.steps + 1)
    steps = np.unique(np.linspace(0, w.steps, 60).astype(int))
    msd = (paths[:, steps] ** 2).mean(axis=0)
    rows = [{"step": int(t), "value": float(v), "series": "measured ⟨x²⟩"} for t, v in zip(steps, msd)]
    rows += [{"step": int(t), "value": float(t), "series": "theory: t"} for t in steps]

    base = alt.Chart(alt.Data(values=rows)).encode(
        x=alt.X("step:Q", title="step"),
        y=alt.Y("value:Q", title="mean squared distance"),
        color=alt.Color("series:N", title=None, legend=alt.Legend(orient="top-left")),
    )
    chart = (base.mark_line() + base.mark_point(size=30, filled=True).encode(
        tooltip=[alt.Tooltip("series:N"), alt.Tooltip("step:Q"), alt.Tooltip("value:Q", format=".1f")],
    )).properties(title=f"Mean squared distance grows as t ({w.walkers} walkers)").interactive()
    report.figure(chart)
