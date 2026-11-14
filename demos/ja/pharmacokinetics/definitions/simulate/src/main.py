"""A one-compartment oral model of a fictional drug, solved numerically dose by
dose. Teaching model only: not medical advice or dosing guidance."""
import tsugumi
import math

import numpy as np
import plotly.graph_objects as go
from plotly.subplots import make_subplots
from scipy.integrate import solve_ivp

from inputs.settings.pk.Settings import Settings
from tsugumi import Failure

STEP_H = 0.1
PLOT_EVERY = 3  # 0.3 h on the plot keeps the figure's JSON small


def simulate(dose, tau, ke, ka, volume, fraction, t_grid):
    """C on t_grid, and C just before the second and the last dose."""
    def rhs(_, y):
        gut, conc = y
        return [-ka * gut, ka * gut / volume - ke * conc]

    end = t_grid[-1]
    doses = int(math.ceil(end / tau - 1e-9))
    conc = np.zeros_like(t_grid)
    y = np.array([0.0, 0.0])
    troughs = []
    for n in range(doses):
        y[0] += fraction * dose
        t0, t1 = n * tau, min((n + 1) * tau, end)
        inside = (t_grid >= t0 - 1e-9) & (t_grid < t1 - 1e-9)
        times = np.append(np.clip(t_grid[inside], t0, t1), t1)
        sol = solve_ivp(rhs, (t0, t1), y, t_eval=times, rtol=1e-10, atol=1e-10, method="RK45")
        if not sol.success:
            raise Failure("worker_failed", f"solve_ivp failed: {sol.message}")
        conc[inside] = sol.y[1][:-1]
        y = sol.y[:, -1]
        troughs.append(y[1])
    conc[-1] = y[1]
    # troughs[n] is C at the end of interval n, just before dose n + 1.
    last_dose = (doses - 1) * tau
    return conc, troughs[0], troughs[doses - 2], last_dose


def main(inputs, report):
    s = inputs.input("settings", Settings)
    doses = s.DosesMgAsNumpy()
    intervals = s.IntervalsHAsNumpy()
    if len(doses) != len(intervals) or len(doses) == 0:
        raise Failure("output_invalid", "doses_mg and intervals_h must have the same length, at least 1")
    if np.any(intervals <= 0) or s.HalfLifeH() <= 0:
        raise Failure("output_invalid", "intervals and the half-life must be positive")
    ke = math.log(2) / s.HalfLifeH()
    ka, volume, fraction = s.KaPerH(), s.VolumeL(), s.Bioavailability()
    end = s.Days() * 24
    if end < 2 * intervals.max():
        raise Failure("output_invalid", f"days must cover two of the longest interval ({intervals.max()} h)")

    t = np.arange(0, end + STEP_H / 2, STEP_H)
    curves, first, last, last_dose = [], [], [], []
    for dose, tau in zip(doses, intervals):
        c, a, b, d = simulate(dose, tau, ke, ka, volume, fraction, t)
        curves.append(c)
        first.append(a)
        last.append(b)
        last_dose.append(d)
        report.log(str(f"dose={dose} tau={tau} doses_given={int(d / tau) + 1} first_trough={a:.5f} last_trough={b:.5f} peak={c.max():.5f}"))
    curves = np.array(curves)

    rows = (len(doses) + 1) // 2
    cols = 2 if len(doses) > 1 else 1
    fig = make_subplots(rows=rows, cols=cols, shared_xaxes=True, shared_yaxes=True, vertical_spacing=0.09, horizontal_spacing=0.04,
                        subplot_titles=[f"{d:g} mg を {i:g} 時間ごと" for d, i in zip(doses, intervals)])
    for k in range(len(doses)):
        fig.add_trace(go.Scatter(x=t[::PLOT_EVERY], y=np.round(curves[k][::PLOT_EVERY], 4), mode="lines", line=dict(color="#2a7ab9", width=1.6),
                                 showlegend=False, name=f"{doses[k]:g} mg / {intervals[k]:g} h",
                                 hovertemplate="%{x:.1f} 時間: %{y:.2f} mg/L<extra></extra>"),
                      row=k // 2 + 1, col=k % 2 + 1)
    # After the traces: Plotly skips the subplots that are still empty.
    fig.add_hrect(y0=s.LowMgL(), y1=s.HighMgL(), row="all", col="all", fillcolor="#3a9a4a", opacity=0.14, line_width=0)
    fig.update_xaxes(title_text="時間 (h)", row=rows)
    fig.update_yaxes(range=[0, 1.05 * max(curves.max(), s.HighMgL())])  # the same scale in every panel
    fig.update_yaxes(title_text="濃度 (mg/L)", col=1)
    fig.update_layout(
        title=dict(text=f"架空の薬物ツグミシンの濃度（半減期 {s.HalfLifeH():g} 時間、緑は作り物の有効域 {s.LowMgL():g}–{s.HighMgL():g} mg/L）", font=dict(size=14)),
        template="plotly_white", margin=dict(l=60, r=20, t=70, b=50))
    report.figure(fig)
    vec = {'doses': np.asarray(doses, dtype=np.float64), 'intervals': np.asarray(intervals, dtype=np.float64), 't': t, 'conc': curves.ravel(), 'first': np.array(first), 'last': np.array(last), 'last_dose': np.array(last_dose)}
    _out_value = {}
    _out_value['doses_mg'] = vec['doses']
    _out_value['intervals_h'] = vec['intervals']
    _out_value['half_life_h'] = s.HalfLifeH()
    _out_value['ka_per_h'] = ka
    _out_value['volume_l'] = volume
    _out_value['bioavailability'] = fraction
    _out_value['low_mg_l'] = s.LowMgL()
    _out_value['high_mg_l'] = s.HighMgL()
    _out_value['days'] = s.Days()
    _out_value['t_h'] = vec['t']
    _out_value['conc'] = vec['conc']
    _out_value['first_trough'] = vec['first']
    _out_value['last_trough'] = vec['last']
    _out_value['last_dose_h'] = vec['last_dose']
    return {'curves': _out_value}
