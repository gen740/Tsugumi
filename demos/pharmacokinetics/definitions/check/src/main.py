"""The numerical solution against closed forms: the superposition of single-dose
responses, the steady-state accumulation ratio and the steady-state mean."""
import json
import math

import numpy as np

from inputs.curves.pk.Curves import Curves
from tsugumi import Failure


def superposition(t, dose, tau, ke, ka, volume, fraction):
    """C(t) = sum over doses of F D ka / (V (ka - ke)) (exp(-ke s) - exp(-ka s)), s = t - n tau."""
    amplitude = fraction * dose * ka / (volume * (ka - ke))
    total = np.zeros_like(t)
    for n in range(int(math.ceil(t[-1] / tau - 1e-9))):
        s = np.clip(t - n * tau, 0, None)
        total += amplitude * (np.exp(-ke * s) - np.exp(-ka * s))
    return total


def main(ctx):
    c = ctx.input("curves", Curves)
    doses, intervals = c.DosesMgAsNumpy(), c.IntervalsHAsNumpy()
    ke = math.log(2) / c.HalfLifeH()
    ka, volume, fraction = c.KaPerH(), c.VolumeL(), c.Bioavailability()
    if abs(ka - ke) < 1e-9:
        raise Failure("output_invalid", "ka equals ke: the closed form here divides by ka - ke")
    t = c.THAsNumpy()
    conc = c.ConcAsNumpy().reshape(len(doses), len(t))
    first, last, last_dose = c.FirstTroughAsNumpy(), c.LastTroughAsNumpy(), c.LastDoseHAsNumpy()

    lines = [
        "**数値解（solve_ivp）と閉じた式の照合**（架空の薬物、教育用のモデル）",
        "",
        "| 投与 | 最大の差 (mg/L) | 蓄積比 $R$ 数値 | $R$ 閉じた式 | $1/(1-e^{-k_e\\tau})$ | 平均濃度 数値 | $F D / (k_e V \\tau)$ | 定常への到達 |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    rows = []
    for k, (dose, tau) in enumerate(zip(doses, intervals)):
        exact = superposition(t, dose, tau, ke, ka, volume, fraction)
        gap = float(np.abs(conc[k] - exact).max())
        if gap > 1e-5 * max(exact.max(), 1e-9):
            raise Failure("output_invalid", f"{dose} mg every {tau} h: the numerical solution is {gap} mg/L off the superposition")

        # Trough at steady state over the trough after the first dose.
        ek, ea = math.exp(-ke * tau), math.exp(-ka * tau)
        ratio_exact = (ek / (1 - ek) - ea / (1 - ea)) / (ek - ea)
        ratio_numeric = float(last[k] / first[k])
        ratio_simple = 1 / (1 - ek)
        reached = 1 - math.exp(-ke * last_dose[k])
        if reached > 0.999 and abs(ratio_numeric / ratio_exact - 1) > 0.01:
            raise Failure("output_invalid", f"{dose} mg every {tau} h: accumulation {ratio_numeric} against {ratio_exact}")

        mask = (t >= last_dose[k] - tau - 1e-9) & (t <= last_dose[k] + 1e-9)
        mean_numeric = float(np.trapezoid(conc[k][mask], t[mask]) / (t[mask][-1] - t[mask][0]))
        mean_exact = fraction * dose / (ke * volume * tau)
        lines.append(f"| {dose:g} mg / {tau:g} h | {gap:.1e} | {ratio_numeric:.4f} | {ratio_exact:.4f} | {ratio_simple:.4f} | "
                     f"{mean_numeric:.3f} | {mean_exact:.3f} | {100 * reached:.2f}% |")
        rows.append({"dose": float(dose), "tau": float(tau), "gap": gap, "ratio_numeric": ratio_numeric, "ratio_exact": ratio_exact,
                     "ratio_simple": ratio_simple, "mean_numeric": mean_numeric, "mean_exact": mean_exact, "reached": reached})
    lines += ["", "$R$ は最初の谷に対する最後の谷の比です。到達が 99.9% に満たないときは、$R$ の差で失敗にはしません（`days` を延ばすと近づきます）。"]
    ctx.display_markdown("\n".join(lines))
    print("result: " + json.dumps(rows))
