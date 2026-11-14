"""Lipinski's rule of five, with his counts: NH + OH for donors, N + O for acceptors."""
import numpy as np


RULES = [("molecular weight > 500", "mw", 500), ("logP > 5", "logp", 5), ("NH+OH > 5", "nh_oh", 5), ("N+O > 10", "n_o", 10)]


def cell(value, limit, digits=0):
    text = f"{value:.{digits}f}"
    return f"**{text}**" if value > limit else text


def main(inputs, report):
    m = inputs.molecules
    limit = inputs.params.maxViolations
    n = len(m.names)
    names = [m.names[i].decode() for i in range(n)]
    formula = [m.formula[i].decode() for i in range(n)]
    values = dict(mw=m.mw, logp=m.logp, nh_oh=m.nhOh, n_o=m.nO)
    broken = [[label for label, key, top in RULES if values[key][i] > top] for i in range(n)]
    violations = np.array([len(b) for b in broken], dtype=np.int32)
    passed = violations <= limit
    report.log(f"{int(passed.sum())} of {n} pass with at most {limit} violations; violations "
          + ", ".join(f"{names[i]} {violations[i]}" for i in range(n)))

    lines = [f"**{int(passed.sum())} of {n} molecules pass** (up to {limit} violations; values over the limit in bold)", "",
             "| Name | Molecular weight | logP | NH+OH | N+O | Violations | Verdict |", "| --- | --- | --- | --- | --- | --- | --- |"]
    for i in range(n):
        lines.append(f"| {names[i]} | {cell(values['mw'][i], 500, 1)} | {cell(values['logp'][i], 5, 2)} | "
                     f"{cell(values['nh_oh'][i], 5)} | {cell(values['n_o'][i], 10)} | {violations[i]} | "
                     f"{'pass' if passed[i] else '**fail**'} |")
    report.markdown("\n".join(lines))
    names_v = list(names)
    formula_v = list(formula)
    broken_v = list([', '.join(x) for x in broken])
    doubles = [v for v in (m.mw, m.logp, m.tpsa)]
    violations_v = violations
    return {'checked': {
        'names': names_v,
        'formula': formula_v,
        'mw': doubles[0],
        'logp': doubles[1],
        'tpsa': doubles[2],
        'violations': violations_v,
        'broken': broken_v,
        'max_violations': limit,
    }}
