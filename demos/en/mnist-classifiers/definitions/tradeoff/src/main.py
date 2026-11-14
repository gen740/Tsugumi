"""Accuracy against fit time and against predict time, as an interactive Plotly figure."""
import numpy as np
import plotly.graph_objects as go
from plotly.subplots import make_subplots


def frontier(times, accuracy):
    """The indices no other model beats on both: faster and at least as accurate."""
    chosen, best = [], -1.0
    for i in np.argsort(times, kind="stable"):
        if accuracy[i] > best:
            chosen.append(int(i))
            best = accuracy[i]
    return chosen


def main(inputs, report):
    c = inputs.comparison
    models = [c.models[i].decode() for i in range(len(c.models))]
    accuracy = c.accuracy
    fit = c.fitSeconds
    predict = c.predictSeconds

    fig = make_subplots(rows=1, cols=2, shared_yaxes=True, horizontal_spacing=0.04,
                        subplot_titles=("Accuracy and training time", "Accuracy and prediction time"))
    for col, times, word in ((1, fit, "Training"), (2, predict, "Prediction")):
        best = frontier(times, accuracy)
        fig.add_trace(go.Scatter(x=times[best], y=accuracy[best], mode="lines", line=dict(color="#bbbbbb", dash="dot"),
                                 hoverinfo="skip", showlegend=False), row=1, col=col)
        # Names above and below in turn along the axis, so that near neighbours stay readable.
        rank = np.argsort(np.argsort(times))
        positions = ["top center" if r % 2 == 0 else "bottom center" for r in rank]
        fig.add_trace(go.Scatter(
            x=times, y=accuracy, mode="markers+text", text=models, textposition=positions, cliponaxis=False,
            marker=dict(size=11, color="#c0392b"), showlegend=False,
            customdata=np.stack([fit, predict], axis=1),
            hovertemplate="%{text}<br>accuracy %{y:.4f}<br>training %{customdata[0]:.2f} s<br>prediction %{customdata[1]:.3f} s<extra></extra>",
        ), row=1, col=col)
        fig.update_xaxes(type="log", title_text=f"{word} CPU seconds (log)", row=1, col=col)
    fig.update_yaxes(title_text="Test accuracy", row=1, col=1)
    fig.update_layout(template="plotly_white", margin=dict(l=60, r=40, t=40, b=50),
                      yaxis=dict(range=[accuracy.min() - 0.01, accuracy.max() + 0.01]))
    report.figure(fig)
    report.log(" ".join(str(value) for value in ("frontier by fit time:", [models[i] for i in frontier(fit, accuracy)],)))
    report.log(" ".join(str(value) for value in ("frontier by predict time:", [models[i] for i in frontier(predict, accuracy)],)))
