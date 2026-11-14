"""Least-squares fit of the four-parameter logistic curve, one compound at a time."""
import matplotlib.pyplot as plt
import numpy as np
from scipy.optimize import curve_fit

from tsugumi import Failure

COLORS = ["#2a78d6", "#eb6834"]


def hill4(x, bottom, top, log_ec50, hill):
    return bottom + (top - bottom) / (1 + 10 ** ((log_ec50 - np.log10(x)) * hill))


def start_values(x, y):
    """Plateaus from the data, EC50 where the mean response crosses halfway."""
    mean = y.reshape(len(np.unique(x)), -1).mean(axis=1)
    conc = np.unique(x)
    low, high = mean.min(), mean.max()
    return [low, high, np.log10(conc[np.argmin(np.abs(mean - (low + high) / 2))]), 1.0]


def main(inputs, report):
    plate = inputs.plate
    conc = plate.conc
    reps = plate.replicates
    n = len(plate.names)
    response = plate.response.reshape(n, len(conc), reps)
    x = np.repeat(conc, reps)
    bounds = ([-50, -50, np.log10(conc[0]) - 3, 0.1], [150, 400, np.log10(conc[-1]) + 3, 8])

    params, residual_sd = [], []
    fig, ax = plt.subplots(figsize=(7, 4.6))
    grid = np.geomspace(conc[0] / 2, conc[-1] * 2, 200)
    for i in range(n):
        name = plate.names[i].decode()
        y = response[i].ravel()
        try:
            est, _ = curve_fit(hill4, x, y, p0=start_values(x, y), bounds=bounds, max_nfev=2000)
        except RuntimeError as e:
            raise Failure("output_invalid", f"compound {name}: the fit did not converge ({e})")
        sd = np.sqrt(np.sum((y - hill4(x, *est)) ** 2) / max(len(y) - 4, 1))
        params.append(est)
        residual_sd.append(sd)
        report.log(f"{name}: bottom {est[0]:.2f} top {est[1]:.2f} EC50 {10 ** est[2]:.4g} uM hill {est[3]:.3f} residual sd {sd:.2f}")
        ax.plot(x, y, "o", ms=4, color=COLORS[i], alpha=0.6, mec="none")
        ax.plot(grid, hill4(grid, *est), color=COLORS[i], lw=2,
                label=f"{name}: EC50 = {10 ** est[2]:.3g} µM, h = {est[3]:.2f}, 上限 {est[1]:.0f}, 下限 {est[0]:.0f}")
    ax.set_xscale("log")
    ax.set_xlabel("濃度 (µM)")
    ax.set_ylabel("反応 (%)")
    ax.set_title(f"4PL の当てはめ（合成データ）: 残差 SD {np.mean(residual_sd):.2f}、与えたノイズ SD {plate.noiseSd:g}")
    ax.grid(alpha=0.25)
    ax.spines[["top", "right"]].set_visible(False)
    ax.legend(frameon=False, loc="upper center", bbox_to_anchor=(0.5, -0.16), ncol=1)
    fig.tight_layout()
    report.figure()
    params_v = np.array(params).ravel()
    sd_v = np.array(residual_sd)
    return {'fit': {
        'params': params_v,
        'residual_sd': sd_v,
    }}
