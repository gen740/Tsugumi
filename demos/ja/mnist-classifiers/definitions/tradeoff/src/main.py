"""Accuracy against fit time and against predict time, as an interactive Plotly figure."""
import numpy as np
import plotly.graph_objects as go
from plotly.subplots import make_subplots

from inputs.comparison.mnist.Comparison import Comparison


def frontier(times, accuracy):
    """The indices no other model beats on both: faster and at least as accurate."""
    chosen, best = [], -1.0
    for i in np.argsort(times, kind="stable"):
        if accuracy[i] > best:
            chosen.append(int(i))
            best = accuracy[i]
    return chosen


def main(ctx):
    c = ctx.input("comparison", Comparison)
    models = [c.Models(i).decode() for i in range(c.ModelsLength())]
    accuracy = c.AccuracyAsNumpy()
    fit = c.FitSecondsAsNumpy()
    predict = c.PredictSecondsAsNumpy()

    fig = make_subplots(rows=1, cols=2, shared_yaxes=True, horizontal_spacing=0.04,
                        subplot_titles=("正解率 と 学習時間", "正解率 と 予測時間"))
    for col, times, word in ((1, fit, "学習"), (2, predict, "予測")):
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
            hovertemplate="%{text}<br>正解率 %{y:.4f}<br>学習 %{customdata[0]:.2f} 秒<br>予測 %{customdata[1]:.3f} 秒<extra></extra>",
        ), row=1, col=col)
        fig.update_xaxes(type="log", title_text=f"{word} CPU 秒（対数）", row=1, col=col)
    fig.update_yaxes(title_text="テスト正解率", row=1, col=1)
    fig.update_layout(template="plotly_white", margin=dict(l=60, r=40, t=40, b=50),
                      yaxis=dict(range=[accuracy.min() - 0.01, accuracy.max() + 0.01]))
    fig.show()
    print("frontier by fit time:", [models[i] for i in frontier(fit, accuracy)])
    print("frontier by predict time:", [models[i] for i in frontier(predict, accuracy)])
