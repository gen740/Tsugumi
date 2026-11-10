"""Reference roots, found without Newton's method: numpy.roots takes the
eigenvalues of the companion matrix. Roots closer than CLUSTER are one multiple
root (rounding splits a double root by about 1e-8, a triple one by about 1e-5)."""
import json

import numpy as np

from inputs.settings.newton.Settings import Settings
import outputs.problem.newton.Problem as out
from tsugumi import Failure

CLUSTER = 1e-3


def cluster(values):
    groups = []
    for v in values:
        for g in groups:
            if abs(np.mean(g) - v) < CLUSTER:
                g.append(v)
                break
        else:
            groups.append([v])
    groups = [(complex(np.mean(g)), len(g)) for g in groups]
    return sorted(groups, key=lambda g: (round(g[0].real, 9), round(g[0].imag, 9)))


def tex_poly(coeffs):
    degree = len(coeffs) - 1
    text = ""
    for i, a in enumerate(coeffs):
        if a == 0:
            continue
        power = degree - i
        sign = "-" if a < 0 else ("+" if text else "")
        size = "" if abs(a) == 1 and power > 0 else f"{abs(a):g}"
        z = "" if power == 0 else ("z" if power == 1 else f"z^{{{power}}}")
        text += f" {sign} {size}{z}" if text else f"{sign}{size}{z}"
    return text


def show(z):
    return f"{z.real:.6g} {'+' if z.imag >= 0 else '-'} {abs(z.imag):.6g}i"


def main(ctx):
    s = ctx.input("settings", Settings)
    coeffs = s.CoeffsAsNumpy()
    if len(coeffs) < 2 or coeffs[0] == 0:
        raise Failure("output_invalid", "coeffs must list a polynomial of degree >= 1, highest power first, leading coefficient not 0")

    raw = np.roots(coeffs)
    roots = cluster(raw)
    critical = cluster(np.roots(np.polyder(coeffs))) if len(coeffs) > 2 else []

    # Vieta: the sum and the product of the roots come from the coefficients alone.
    sum_gap = abs(sum(m * r for r, m in roots) + coeffs[1] / coeffs[0])
    product = np.prod([r ** m for r, m in roots])
    product_gap = abs(product - (-1) ** (len(coeffs) - 1) * coeffs[-1] / coeffs[0])
    if max(sum_gap, product_gap) > 1e-6 * (1 + np.abs(coeffs).max()):
        raise Failure("output_invalid", f"the roots break Vieta's formulas: sum off by {sum_gap}, product by {product_gap}")

    lines = [
        f"**$p(z) = {tex_poly(coeffs)}$**",
        "",
        "| Root | Value | Multiplicity | $|p(r)|$ |",
        "| --- | --- | --- | --- |",
    ]
    for k, (r, m) in enumerate(roots):
        lines.append(f"| $r_{k}$ | ${show(r)}$ | {m} | {abs(np.polyval(coeffs, r)):.1e} |")
    lines += ["", f"**Relations between roots and coefficients**: sum off by {sum_gap:.1e}, product off by {product_gap:.1e}."]

    # z^n = c has the exact roots c^(1/n) e^(2 pi i k / n).
    n = len(coeffs) - 1
    exact_gap = None
    if n >= 1 and not np.any(coeffs[1:-1]) and coeffs[-1] != 0:
        c = complex(-coeffs[-1] / coeffs[0])
        exact = [abs(c) ** (1 / n) * np.exp(1j * (np.angle(c) + 2 * np.pi * k) / n) for k in range(n)]
        exact_gap = max(min(abs(r - e) for r, _ in roots) for e in exact)
        if exact_gap > 1e-9:
            raise Failure("output_invalid", f"the roots differ from the exact z^n = c roots by {exact_gap}")
        lines.append(f"Largest difference from the **closed form** $c^{{1/{n}}} e^{{2\\pi i k/{n}}}$: {exact_gap:.1e}.")
    if critical:
        where = ", ".join(f"${show(r)}$" + (f" (multiplicity {m})" if m > 1 else "") for r, m in critical)
        lines += ["", f"**Critical points** $p'(z) = 0$: {where}. Here the iteration's formula cannot divide."]
    ctx.display_markdown("\n".join(lines))
    print("result: " + json.dumps({"roots": [[r.real, r.imag, m] for r, m in roots],
                                   "exact_gap": None if exact_gap is None else float(exact_gap)}))

    b = ctx.builder()
    label = b.CreateString(tex_poly(coeffs))
    vec_coeffs = b.CreateNumpyVector(np.asarray(coeffs, dtype=np.float64))
    roots_re = b.CreateNumpyVector(np.array([r.real for r, _ in roots]))
    roots_im = b.CreateNumpyVector(np.array([r.imag for r, _ in roots]))
    multiplicity = b.CreateNumpyVector(np.array([m for _, m in roots], dtype=np.uint64))
    critical_re = b.CreateNumpyVector(np.array([r.real for r, _ in critical], dtype=np.float64))
    critical_im = b.CreateNumpyVector(np.array([r.imag for r, _ in critical], dtype=np.float64))
    out.Start(b)
    out.AddLabel(b, label)
    out.AddCoeffs(b, vec_coeffs)
    out.AddRootsRe(b, roots_re)
    out.AddRootsIm(b, roots_im)
    out.AddMultiplicity(b, multiplicity)
    out.AddCriticalRe(b, critical_re)
    out.AddCriticalIm(b, critical_im)
    out.AddDamping(b, s.Damping())
    out.AddTolerance(b, s.Tolerance())
    out.AddMaxIter(b, s.MaxIter())
    ctx.output("problem", b, out.End(b))
