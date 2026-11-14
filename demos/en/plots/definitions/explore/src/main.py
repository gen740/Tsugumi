"""Plain Plotly, as written for a notebook or a browser: report.figure(fig) puts the
interactive figure in the Card's display (zoom, hover and legend work in the
expanded view)."""
import numpy as np
import plotly.graph_objects as go


def main(inputs, report):
    w = inputs.walks
    paths = w.positions.reshape(w.walkers, w.steps + 1)
    final = paths[:, -1]
    reach = np.abs(paths).max(axis=1)
    above = (paths[:, 1:] > 0).mean(axis=1)  # share of the time spent above 0

    fig = go.Figure(go.Scatter(
        x=final, y=reach, mode="markers",
        marker=dict(size=7, color=above, colorscale="Viridis", cmin=0, cmax=1,
                    colorbar=dict(title="time<br>above 0"), line=dict(width=0.5, color="white")),
        customdata=np.stack([np.arange(len(final)), np.round(100 * above)], axis=1),
        hovertemplate="walker %{customdata[0]}<br>ends at %{x}<br>reaches %{y}<br>"
                      "time above 0: %{customdata[1]}%<extra></extra>",
    ))
    fig.update_layout(
        title=f"Where {w.walkers} walkers end and how far they reach",
        xaxis_title=f"position after {w.steps} steps", yaxis_title="largest distance from 0",
        template="plotly_white", margin=dict(l=60, r=20, t=50, b=50),
    )
    report.figure(fig)
