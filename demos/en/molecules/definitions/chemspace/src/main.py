"""Molecular weight against logP, interactive: hover gives the name and what it
broke. Names are written on the plot only where they do not collide."""
import numpy as np
import plotly.graph_objects as go

from inputs.checked.mol.Checked import Checked


def labelled(mw, logp, first):
    """Indices to write a name for: `first` in order, then whatever keeps its distance from those."""
    chosen = []
    for i in sorted(range(len(mw)), key=lambda i: (i not in first, mw[i])):
        if all(abs(np.log10(mw[i] / mw[j])) > 0.13 or abs(logp[i] - logp[j]) > 0.7 for j in chosen):
            chosen.append(i)
    return set(chosen)


def main(ctx):
    c = ctx.input("checked", Checked)
    n = c.NamesLength()
    names = [c.Names(i).decode() for i in range(n)]
    mw, logp, tpsa = c.MwAsNumpy(), c.LogpAsNumpy(), c.TpsaAsNumpy()
    limit = c.MaxViolations()
    passed = [c.Violations(i) <= limit for i in range(n)]
    shown = labelled(mw, logp, {i for i in range(n) if not passed[i]})
    hover = [f"formula {c.Formula(i).decode()}<br>TPSA {tpsa[i]:.1f} Å²<br>"
             + ("no violations" if c.Violations(i) == 0 else f"violates: {c.Broken(i).decode()}") for i in range(n)]

    fig = go.Figure()
    low, high = min(logp.min(), 0) - 1, max(logp.max(), 5) + 1
    left, right = min(mw.min(), 500) * 0.8, max(mw.max(), 500) * 1.25
    limits = dict(mode="lines", line=dict(color="#888888", dash="dash", width=1), hoverinfo="skip", showlegend=False)
    fig.add_trace(go.Scatter(x=[500, 500], y=[low, high], **limits))
    fig.add_trace(go.Scatter(x=[left, right], y=[5, 5], **limits))
    for ok, color, label in ((True, "#2a78d6", "pass"), (False, "#eb6834", "fail")):
        chosen = [i for i in range(n) if passed[i] == ok]
        fig.add_trace(go.Scatter(
            x=mw[chosen], y=logp[chosen], mode="markers+text", name=label,
            text=[names[i] if i in shown else "" for i in chosen], textposition="top center", textfont=dict(size=11),
            marker=dict(size=10, color=color, line=dict(width=1.5, color="white")),
            customdata=[[names[i], hover[i]] for i in chosen],
            hovertemplate="<b>%{customdata[0]}</b><br>molecular weight %{x:.1f}<br>logP %{y:.2f}<br>%{customdata[1]}<extra></extra>",
        ))
    fig.update_layout(
        title=dict(text=f"Molecular weight and logP (dashed lines are the Rule of Five's limits; passing with up to {limit} violations. Hover for names)", font_size=14),
        xaxis=dict(title="Molecular weight (log axis)", type="log", range=[np.log10(left), np.log10(right)],
                   tickvals=[100, 150, 200, 300, 500, 800, 1200]),
        yaxis=dict(title="logP (Crippen)", range=[low, high]),
        template="plotly_white", margin=dict(l=60, r=20, t=50, b=50),
        legend=dict(orientation="h", y=-0.18),
    )
    fig.show()
