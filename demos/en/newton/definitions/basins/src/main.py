"""The basins of attraction: every point of a grid is an initial value. Left,
which root it reaches (darker = more iterations); right, how many iterations."""
import json

import matplotlib.pyplot as plt
import numpy as np

from inputs.problem.newton.Problem import Problem
from tsugumi import Failure

EPS = np.finfo(float).eps
COLORS = np.array([[0.13, 0.47, 0.71], [0.89, 0.47, 0.13], [0.20, 0.63, 0.30], [0.60, 0.40, 0.75],
                   [0.84, 0.25, 0.28], [0.55, 0.34, 0.29], [0.89, 0.47, 0.76], [0.50, 0.50, 0.50]])


def newton_grid(coeffs, z, damping, tolerance, max_iter):
    """The final z, the iteration count and whether each point converged."""
    derivative = np.polyder(coeffs)
    magnitudes = np.abs(coeffs)
    count = np.zeros(z.shape, dtype=np.int32)
    done = np.zeros(z.shape, dtype=bool)
    active = np.arange(z.size)
    z = z.ravel().copy()
    flat_count, flat_done = count.ravel(), done.ravel()
    for _ in range(max_iter):
        current = z[active]
        value = np.polyval(coeffs, current)
        slope = np.polyval(derivative, current)
        small = np.abs(value) <= 4 * EPS * np.polyval(magnitudes, np.abs(current))
        flat_done[active[small]] = True
        stuck = slope == 0
        moving = ~(small | stuck)
        active = active[moving]
        if active.size == 0:
            break
        step = damping * value[moving] / slope[moving]
        z[active] = current[moving] - step
        flat_count[active] += 1
        arrived = np.abs(step) < tolerance
        flat_done[active[arrived]] = True
        keep = ~arrived & np.isfinite(z[active])
        active = active[keep]
        if active.size == 0:
            break
    return z.reshape(count.shape), count, done


def main(ctx):
    p = ctx.input("problem", Problem)
    coeffs = p.CoeffsAsNumpy().copy()
    roots = p.RootsReAsNumpy() + 1j * p.RootsImAsNumpy()
    critical = p.CriticalReAsNumpy() + 1j * p.CriticalImAsNumpy()
    params = ctx.params()
    n, cx, cy, half = int(params.Grid()), params.CenterRe(), params.CenterIm(), params.HalfWidth()

    x = np.linspace(cx - half, cx + half, n)
    y = np.linspace(cy - half, cy + half, n)
    z0 = x[None, :] + 1j * y[:, None]
    z, count, done = newton_grid(coeffs, z0, p.Damping(), p.Tolerance(), int(p.MaxIter()))

    distance = np.abs(z[..., None] - roots)
    nearest = np.nan_to_num(distance, nan=np.inf).argmin(axis=-1)
    gap = np.take_along_axis(distance, nearest[..., None], axis=-1)[..., 0]
    if done.any() and gap[done].max() > 1e-3:
        raise Failure("output_invalid", f"a converged point ended {gap[done].max()} from every reference root")

    shares = [float(((nearest == k) & done).mean()) for k in range(len(roots))]
    unconverged = float((~done).mean())

    image = np.zeros((n, n, 3))
    top = max(int(np.percentile(count[done], 99)) if done.any() else 1, 1)
    shade = 1 - 0.75 * np.minimum(count / top, 1) ** 0.6
    for k in range(len(roots)):
        mask = (nearest == k) & done
        image[mask] = COLORS[k % len(COLORS)] * shade[mask][:, None]

    extent = [x[0], x[-1], y[0], y[-1]]
    fig, (left, right) = plt.subplots(1, 2, figsize=(11, 5.5), constrained_layout=True)
    left.imshow(image, origin="lower", extent=extent, interpolation="nearest")
    left.set_title("Which root it reaches (darker is more iterations, black did not converge)", fontsize=11)
    shown = right.imshow(np.minimum(count, top), origin="lower", extent=extent, cmap="magma", interpolation="nearest")
    right.set_title("Number of iterations", fontsize=11)
    fig.colorbar(shown, ax=right, shrink=0.82, label=f"Iterations ({top} and above share a colour)")
    for ax in (left, right):
        inside = lambda w: extent[0] <= w.real <= extent[1] and extent[2] <= w.imag <= extent[3]
        for r in filter(inside, roots):
            ax.plot(r.real, r.imag, marker="*", markersize=13, markerfacecolor="white", markeredgecolor="black", linestyle="none")
        for c in filter(inside, critical):
            ax.plot(c.real, c.imag, marker="x", markersize=9, markeredgewidth=2, color="white", linestyle="none")
        ax.set_xlabel("Re $z_0$")
        ax.set_ylabel("Im $z_0$")
    # A line with $...$ is drawn by mathtext, which has no Japanese glyphs: keep them on separate lines.
    share_text = ", ".join(f"r{k} {100 * s:.1f}%" for k, s in enumerate(shares))
    fig.suptitle(f"$p(z) = {p.Label().decode()}$,  $\\lambda = {p.Damping():g}$,  {n} × {n}\n"
                 f"★ are roots, × are critical points. {share_text}, not converged {100 * unconverged:.2f}%", fontsize=12)
    plt.show()
    print("result: " + json.dumps({"shares": shares, "unconverged": unconverged, "mean_iterations": float(count.mean()),
                                   "max_iterations": int(count.max()), "worst_gap": float(gap[done].max()) if done.any() else None}))
