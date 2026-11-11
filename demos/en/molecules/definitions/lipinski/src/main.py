"""Lipinski's rule of five, with his counts: NH + OH for donors, N + O for acceptors."""
import numpy as np

from inputs.molecules.mol.Molecules import Molecules
import outputs.checked.mol.Checked as out

RULES = [("molecular weight > 500", "mw", 500), ("logP > 5", "logp", 5), ("NH+OH > 5", "nh_oh", 5), ("N+O > 10", "n_o", 10)]


def strings(b, items):
    offsets = [b.CreateString(s) for s in items]
    b.StartVector(4, len(offsets), 4)
    for o in reversed(offsets):
        b.PrependUOffsetTRelative(o)
    return b.EndVector()


def cell(value, limit, digits=0):
    text = f"{value:.{digits}f}"
    return f"**{text}**" if value > limit else text


def main(ctx):
    m = ctx.input("molecules", Molecules)
    limit = ctx.params().MaxViolations()
    n = m.NamesLength()
    names = [m.Names(i).decode() for i in range(n)]
    formula = [m.Formula(i).decode() for i in range(n)]
    values = dict(mw=m.MwAsNumpy(), logp=m.LogpAsNumpy(), nh_oh=m.NhOhAsNumpy(), n_o=m.NOAsNumpy())
    broken = [[label for label, key, top in RULES if values[key][i] > top] for i in range(n)]
    violations = np.array([len(b) for b in broken], dtype=np.int32)
    passed = violations <= limit
    print(f"{int(passed.sum())} of {n} pass with at most {limit} violations; violations "
          + ", ".join(f"{names[i]} {violations[i]}" for i in range(n)))

    lines = [f"**{int(passed.sum())} of {n} molecules pass** (up to {limit} violations; values over the limit in bold)", "",
             "| Name | Molecular weight | logP | NH+OH | N+O | Violations | Verdict |", "| --- | --- | --- | --- | --- | --- | --- |"]
    for i in range(n):
        lines.append(f"| {names[i]} | {cell(values['mw'][i], 500, 1)} | {cell(values['logp'][i], 5, 2)} | "
                     f"{cell(values['nh_oh'][i], 5)} | {cell(values['n_o'][i], 10)} | {violations[i]} | "
                     f"{'pass' if passed[i] else '**fail**'} |")
    ctx.display_markdown("\n".join(lines))

    b = ctx.builder(4096)
    names_v = strings(b, names)
    formula_v = strings(b, formula)
    broken_v = strings(b, [", ".join(x) for x in broken])
    doubles = [b.CreateNumpyVector(v) for v in (m.MwAsNumpy(), m.LogpAsNumpy(), m.TpsaAsNumpy())]
    violations_v = b.CreateNumpyVector(violations)
    out.Start(b)
    out.AddNames(b, names_v)
    out.AddFormula(b, formula_v)
    out.AddMw(b, doubles[0])
    out.AddLogp(b, doubles[1])
    out.AddTpsa(b, doubles[2])
    out.AddViolations(b, violations_v)
    out.AddBroken(b, broken_v)
    out.AddMaxViolations(b, limit)
    ctx.output("checked", b, out.End(b))
