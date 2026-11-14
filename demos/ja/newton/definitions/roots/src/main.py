"""Reference roots, found without Newton's method: numpy.roots takes the
eigenvalues of the companion matrix. Roots closer than CLUSTER are one multiple
root (rounding splits a double root by about 1e-8, a triple one by about 1e-5)."""
import tsugumi
import json

import numpy as np

from inputs.settings.newton.Settings import Settings
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


def main(inputs, report):
    s = inputs.input("settings", Settings)
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
        "| 根 | 値 | 重複度 | $|p(r)|$ |",
        "| --- | --- | --- | --- |",
    ]
    for k, (r, m) in enumerate(roots):
        lines.append(f"| $r_{k}$ | ${show(r)}$ | {m} | {abs(np.polyval(coeffs, r)):.1e} |")
    lines += ["", f"**根と係数の関係**: 和のずれ {sum_gap:.1e}、積のずれ {product_gap:.1e}。"]

    # z^n = c has the exact roots c^(1/n) e^(2 pi i k / n).
    n = len(coeffs) - 1
    exact_gap = None
    if n >= 1 and not np.any(coeffs[1:-1]) and coeffs[-1] != 0:
        c = complex(-coeffs[-1] / coeffs[0])
        exact = [abs(c) ** (1 / n) * np.exp(1j * (np.angle(c) + 2 * np.pi * k) / n) for k in range(n)]
        exact_gap = max(min(abs(r - e) for r, _ in roots) for e in exact)
        if exact_gap > 1e-9:
            raise Failure("output_invalid", f"the roots differ from the exact z^n = c roots by {exact_gap}")
        lines.append(f"**閉じた式** $c^{{1/{n}}} e^{{2\\pi i k/{n}}}$ との最大の差: {exact_gap:.1e}。")
    if critical:
        where = "、".join(f"${show(r)}$" + (f"（重複度 {m}）" if m > 1 else "") for r, m in critical)
        lines += ["", f"**臨界点** $p'(z) = 0$: {where}。ここでは反復の式が割り算できません。"]
    report.markdown("\n".join(lines))
    report.log(str("result: " + json.dumps({"roots": [[r.real, r.imag, m] for r, m in roots],
                                   "exact_gap": None if exact_gap is None else float(exact_gap)})))
    label = tex_poly(coeffs)
    vec_coeffs = np.asarray(coeffs, dtype=np.float64)
    roots_re = np.array([r.real for (r, _) in roots])
    roots_im = np.array([r.imag for (r, _) in roots])
    multiplicity = np.array([m for (_, m) in roots], dtype=np.uint64)
    critical_re = np.array([r.real for (r, _) in critical], dtype=np.float64)
    critical_im = np.array([r.imag for (r, _) in critical], dtype=np.float64)
    _out_value = {}
    _out_value['label'] = label
    _out_value['coeffs'] = vec_coeffs
    _out_value['roots_re'] = roots_re
    _out_value['roots_im'] = roots_im
    _out_value['multiplicity'] = multiplicity
    _out_value['critical_re'] = critical_re
    _out_value['critical_im'] = critical_im
    _out_value['damping'] = s.Damping()
    _out_value['tolerance'] = s.Tolerance()
    _out_value['max_iter'] = s.MaxIter()
    return {'problem': _out_value}
