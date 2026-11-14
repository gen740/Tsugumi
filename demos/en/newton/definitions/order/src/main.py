"""The order of convergence read off the errors:
q = log(e[k+1] / e[k]) / log(e[k] / e[k-1]), on the last three errors that are
close to the root yet above the rounding floor, and beside it what theory says
for a root of multiplicity m with damping lambda: order 2 when lambda = m,
otherwise order 1 with the rate |1 - lambda / m|."""
import json

import numpy as np

from tsugumi import Failure

EPS = np.finfo(float).eps
NEAR = 0.3


def estimate(e, m):
    """(order, ratio e[k+1] / e[k], constant e[k+1] / e[k]^2) on the last usable errors."""
    floor = 100 * EPS if m == 1 else 10 * EPS ** (1 / m)  # a root of multiplicity m is resolved only to eps^(1/m)
    for k in range(len(e) - 2, 0, -1):
        a, b, c = e[k - 1], e[k], e[k + 1]
        if floor < c and a < NEAR and c < b < a:
            return np.log(c / b) / np.log(b / a), c / b, c / b ** 2
    return None, None, None


def main(inputs, report):
    o = inputs.errors
    coeffs = o.coeffs
    damping = o.damping
    statuses = o.status.decode().split()
    lengths = o.lengths
    all_errors = o.errors
    start = o.startRe + 1j * o.startIm
    root = o.rootRe + 1j * o.rootIm
    mult = o.multiplicity

    lines = [
        f"**Convergence at $\\lambda = {damping:g}$** ($q$ is the order estimated from three errors)",
        "",
        "| $z_0$ | Reaches $r$ | Multiplicity | Iterations | Last error | Order $q$ | $e_{k+1}/e_k$ | $e_{k+1}/e_k^2$ | Theory |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    rows = []
    offset = 0
    for i, n in enumerate(lengths):
        e = all_errors[offset:offset + int(n)]
        offset += int(n)
        m = int(mult[i])
        rate = abs(1 - damping / m)
        theory = "$q = 2$" if rate < 1e-9 else f"$q = 1$, ratio ${rate:.3g}$"
        z = start[i]
        r = root[i]
        where = f"{r.real:.4g} {'+' if r.imag >= 0 else '-'} {abs(r.imag):.4g}i"
        q, ratio, constant = estimate(e, m)
        converged = statuses[i] == "converged"
        if converged and e[-1] > 1e-5 * (1 + abs(r)) * 10 ** (2 * (m - 1)):
            raise Failure("output_invalid", f"z0={z} claims to converge but ends {e[-1]} from the root {r}")
        if not converged:
            why = {"max_iter": "stopped at the iteration limit", "derivative_zero": "stopped at $p' = 0$", "diverged": "diverged"}[statuses[i]]
            lines.append(f"| ${z.real:g} {'+' if z.imag >= 0 else '-'} {abs(z.imag):g}i$ | {why} | | {n - 1} | {e[-1]:.1e} | | | | {theory} |")
        else:
            # The ratio tells on a linear root, the constant on a quadratic one; the other is noise.
            quadratic = rate < 1e-9
            cell = lambda v, f, wanted: "—" if v is None or not wanted else format(v, f)
            lines.append(f"| ${z.real:g} {'+' if z.imag >= 0 else '-'} {abs(z.imag):g}i$ | ${where}$ | {m} | {n - 1} | {e[-1]:.1e} | "
                         f"{cell(q, '.2f', True)} | {cell(ratio, '.3f', not quadratic)} | {cell(constant, '.2f', quadratic)} | {theory} |")
        rows.append({"z0": [z.real, z.imag], "root": [r.real, r.imag], "m": m, "iterations": int(n) - 1, "status": statuses[i],
                     "last_error": float(e[-1]), "q": None if q is None else float(q),
                     "ratio": None if ratio is None else float(ratio), "constant": None if constant is None else float(constant)})

    # For a simple root and lambda = 1, the constant of e[k+1] = C e[k]^2 is |p''(r) / (2 p'(r))|.
    simple = [r for r in rows if r["m"] == 1 and r["status"] == "converged"]
    if simple and damping == 1:
        d1, d2 = np.polyder(coeffs), np.polyder(coeffs, 2)
        shown = sorted({round(abs(np.polyval(d2, complex(*r["root"])) / (2 * np.polyval(d1, complex(*r["root"])))), 3) for r in simple})
        lines += ["", "Theoretical constant at simple roots $|p''(r) / (2 p'(r))|$: " + ", ".join(f"{v:g}" for v in shown) + "."]
    report.markdown("\n".join(lines))
    report.log("result: " + json.dumps(rows))
