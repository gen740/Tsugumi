"""Where Newton's method loses its quadratic convergence or fails, beside a case
where it works. The polynomials are fixed here; settings gives only the tolerance
and the iteration limit."""
import json

import matplotlib.pyplot as plt
import numpy as np

from inputs.problem.newton.Problem import Problem

EPS = np.finfo(float).eps
NEAR = 0.3


def iterate(coeffs, z0, damping, tolerance, max_iter):
    derivative = np.polyder(coeffs)
    path = [complex(z0)]
    z = complex(z0)
    for _ in range(max_iter):
        value = np.polyval(coeffs, z)
        if abs(value) <= 4 * EPS * np.polyval(np.abs(coeffs), abs(z)):
            return path, "converged"
        slope = np.polyval(derivative, z)
        if slope == 0:
            return path, "derivative_zero"
        step = damping * value / slope
        z -= step
        if not np.isfinite(z):
            return path, "diverged"
        path.append(z)
        if abs(step) < tolerance:
            return path, "converged"
    return path, "max_iter"


def floor_of(m):
    """Errors below this are rounding: a root of multiplicity m is resolved only to eps^(1/m)."""
    return 100 * EPS if m == 1 else 10 * EPS ** (1 / m)


def order(e, m):
    floor = floor_of(m)
    for k in range(len(e) - 2, 0, -1):
        a, b, c = e[k - 1], e[k], e[k + 1]
        if floor < c and a < NEAR and c < b < a:
            return float(np.log(c / b) / np.log(b / a))
    return None


def linear_rate(e, m):
    """The median of e[k+1] / e[k] over the stretch where the error shrinks steadily."""
    floor = floor_of(m)
    ratios =[e[k + 1] / e[k] for k in range(1, len(e) - 1) if e[k + 1] > floor and e[k + 1] < e[k]]
    return float(np.median(ratios)) if len(ratios) >= 6 else None


def main(inputs, report):
    p = inputs.input("problem", Problem)
    offset = inputs.params.offset
    tolerance, max_iter = p.Tolerance(), int(p.MaxIter())
    cube = [1.0, 0.0, 0.0, -1.0]
    double = [1.0, 0.0, -3.0, 2.0]  # (z - 1)^2 (z + 2)
    cycle = [1.0, 0.0, -2.0, 2.0]  # 0 -> 1 -> 0 -> ...
    cases = [
        ("Reference: simple root", cube, 1.5, 1.0, 1, "#2a7ab9"),
        ("Near the critical point 0", cube, offset, 1.0, 1, "#d9622b"),
        ("Multiple root (z − 1)²(z + 2)", double, 2.0, 1.0, 2, "#3a9a4a"),
        ("Multiple root with λ = 2", double, 2.0, 2.0, 2, "#8a5ec2"),
        ("Cycle z³ − 2z + 2", cycle, 0.0, 1.0, 1, "#c23b4b"),
    ]

    fig, (ax, table_ax) = plt.subplots(2, 1, figsize=(9, 7.6), height_ratios=[3, 1.5], constrained_layout=True)
    rows, results = [], []
    for name, coeffs, z0, damping, m, color in cases:
        path, status = iterate(np.array(coeffs), z0, damping, tolerance, max_iter)
        roots = np.roots(coeffs)
        e = np.abs(np.array(path) - roots[np.argmin(np.abs(roots - path[-1]))])
        if name.startswith("Cycle"):  # it never arrives: measure to the real root, the only one it could reach
            e = np.abs(np.array(path) - roots[np.argmin(np.abs(roots.imag))])
        ax.semilogy(np.where(e > 0, e, np.nan), color=color, marker="o", markersize=3, linewidth=1.3, label=name)
        q = order(e, m) if status == "converged" else None
        rate = linear_rate(e, m) if status == "converged" else None
        rows.append([name, f"{z0:g}", str(len(path) - 1), f"{e[-1]:.1e}", {"converged": "converged", "max_iter": "not converged (limit)", "derivative_zero": "stopped at p′ = 0", "diverged": "diverged"}[status],
                     "—" if q is None else f"{q:.2f}", "—" if rate is None else f"{rate:.3f}"])
        results.append({"name": name, "z0": z0, "iterations": len(path) - 1, "status": status, "last_error": float(e[-1]), "q": q,
                        "rate": rate, "peak_error": float(e.max())})

    ax.set_xlabel("Iteration k")
    ax.set_ylabel("Error |z − r|")
    ax.set_title("Losing quadratic convergence, and not converging", fontsize=12)
    ax.grid(True, which="major", alpha=0.25)
    ax.legend(fontsize=9, loc="upper right")

    table_ax.axis("off")
    table = table_ax.table(cellText=rows, colLabels=["Case", "z₀", "Iterations", "Last error", "Result", "Order q", "Shrink ratio"],
                           colWidths=[0.34, 0.08, 0.07, 0.13, 0.18, 0.09, 0.09], loc="center", cellLoc="center")
    table.auto_set_font_size(False)
    table.set_fontsize(9)
    table.scale(1, 1.4)
    report.figure()
    report.log(str("result: " + json.dumps(results)))
