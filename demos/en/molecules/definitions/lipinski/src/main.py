"""Lipinski's rule of five, with his counts: NH + OH for donors, N + O for acceptors."""
import tsugumi
import numpy as np

from inputs.molecules.mol.Molecules import Molecules

RULES = [("molecular weight > 500", "mw", 500), ("logP > 5", "logp", 5), ("NH+OH > 5", "nh_oh", 5), ("N+O > 10", "n_o", 10)]


def strings(items):
    offsets = [s for s in items]
    _b_items = []
    for o in reversed(offsets):
        _b_items.append(o)
    return list(reversed(_b_items))


def cell(value, limit, digits=0):
    text = f"{value:.{digits}f}"
    return f"**{text}**" if value > limit else text


def main(inputs, report):
    m = inputs.input("molecules", Molecules)
    limit = inputs.params.maxViolations
    n = m.NamesLength()
    names = [m.Names(i).decode() for i in range(n)]
    formula = [m.Formula(i).decode() for i in range(n)]
    values = dict(mw=m.MwAsNumpy(), logp=m.LogpAsNumpy(), nh_oh=m.NhOhAsNumpy(), n_o=m.NOAsNumpy())
    broken = [[label for label, key, top in RULES if values[key][i] > top] for i in range(n)]
    violations = np.array([len(b) for b in broken], dtype=np.int32)
    passed = violations <= limit
    report.log(str(f"{int(passed.sum())} of {n} pass with at most {limit} violations; violations "
          + ", ".join(f"{names[i]} {violations[i]}" for i in range(n))))

    lines = [f"**{int(passed.sum())} of {n} molecules pass** (up to {limit} violations; values over the limit in bold)", "",
             "| Name | Molecular weight | logP | NH+OH | N+O | Violations | Verdict |", "| --- | --- | --- | --- | --- | --- | --- |"]
    for i in range(n):
        lines.append(f"| {names[i]} | {cell(values['mw'][i], 500, 1)} | {cell(values['logp'][i], 5, 2)} | "
                     f"{cell(values['nh_oh'][i], 5)} | {cell(values['n_o'][i], 10)} | {violations[i]} | "
                     f"{'pass' if passed[i] else '**fail**'} |")
    report.markdown("\n".join(lines))
    names_v = strings(names)
    formula_v = strings(formula)
    broken_v = strings([', '.join(x) for x in broken])
    doubles = [v for v in (m.MwAsNumpy(), m.LogpAsNumpy(), m.TpsaAsNumpy())]
    violations_v = violations
    _out_value = {}
    _out_value['names'] = names_v
    _out_value['formula'] = formula_v
    _out_value['mw'] = doubles[0]
    _out_value['logp'] = doubles[1]
    _out_value['tpsa'] = doubles[2]
    _out_value['violations'] = violations_v
    _out_value['broken'] = broken_v
    _out_value['max_violations'] = limit
    return {'checked': _out_value}
