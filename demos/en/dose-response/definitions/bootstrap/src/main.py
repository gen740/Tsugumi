"""Residual bootstrap: refit on the fitted curve plus resampled residuals, then
take percentile intervals. Says so when an interval leaves the measured range."""
import numpy as np
from scipy.optimize import curve_fit

from inputs.fit.dose.Fit import Fit
from inputs.plate.dose.Plate import Plate
import outputs.intervals.dose.Intervals as out


BLOWN_UP = 30  # an interval this many times wide, leaving the range, does not fix the EC50


def hill4(x, bottom, top, log_ec50, hill):
    return bottom + (top - bottom) / (1 + 10 ** ((log_ec50 - np.log10(x)) * hill))


def strings(b, items):
    offsets = [b.CreateString(s) for s in items]
    b.StartVector(4, len(offsets), 4)
    for o in reversed(offsets):
        b.PrependUOffsetTRelative(o)
    return b.EndVector()


def resample(x, y, est, n_boot, bounds, rng):
    """Bootstrap samples of the parameters, and how many refits failed."""
    curve = hill4(x, *est)
    resid = y - curve
    resid = (resid - resid.mean()) * np.sqrt(len(y) / max(len(y) - 4, 1))
    samples, failed = [], 0
    for _ in range(n_boot):
        fake = curve + rng.choice(resid, size=len(y), replace=True)
        try:
            samples.append(curve_fit(hill4, x, fake, p0=est, bounds=bounds, max_nfev=400)[0])
        except RuntimeError:
            failed += 1
    return np.array(samples), failed


def yes(inside):
    return "inside" if inside else "**outside**"


def main(ctx):
    plate = ctx.input("plate", Plate)
    fit = ctx.input("fit", Fit)
    p = ctx.params()
    n_boot = p.NBoot()
    conc = plate.ConcAsNumpy()
    reps = plate.Replicates()
    n = plate.NamesLength()
    names = [plate.Names(i).decode() for i in range(n)]
    response = plate.ResponseAsNumpy().reshape(n, len(conc), reps)
    estimates = fit.ParamsAsNumpy().reshape(n, 4)
    x = np.repeat(conc, reps)
    bounds = ([-50, -50, np.log10(conc[0]) - 3, 0.1], [150, 400, np.log10(conc[-1]) + 3, 8])
    rng = np.random.default_rng(p.Seed())

    rows, warnings, missed = [], [], 0
    ec50 = np.zeros((n, 3))
    hill = np.zeros((n, 3))
    log_samples, hill_samples = [], []
    for i, name in enumerate(names):
        samples, failed = resample(x, response[i].ravel(), estimates[i], n_boot, bounds, rng)
        # Keep rectangular sample arrays when some refits fail: NaN marks them.
        pad = np.full((failed, 4), np.nan)
        samples = np.vstack([samples, pad]) if failed else samples
        log_low, log_high = np.nanpercentile(samples[:, 2], [2.5, 97.5])
        hill_low, hill_high = np.nanpercentile(samples[:, 3], [2.5, 97.5])
        ec50[i] = [10 ** estimates[i, 2], 10 ** log_low, 10 ** log_high]
        hill[i] = [estimates[i, 3], hill_low, hill_high]
        log_samples.append(samples[:, 2])
        hill_samples.append(samples[:, 3])

        true_ec50, true_hill = plate.Ec50(i), plate.Hill(i)
        ec50_ok = ec50[i, 1] <= true_ec50 <= ec50[i, 2]
        hill_ok = hill_low <= true_hill <= hill_high
        missed += (not ec50_ok) + (not hill_ok)
        rows.append(f"| {name} | EC50 (µM) | {true_ec50:.3g} | {ec50[i, 0]:.3g} | {ec50[i, 1]:.3g} – {ec50[i, 2]:.3g} | "
                    f"{ec50[i, 2] / ec50[i, 1]:.1f}-fold | {yes(ec50_ok)} |")
        rows.append(f"| {name} | Hill slope | {true_hill:.3g} | {hill[i, 0]:.3g} | {hill_low:.3g} – {hill_high:.3g} | "
                    f"difference {hill_high - hill_low:.2f} | {yes(hill_ok)} |")
        print(f"{name}: EC50 {ec50[i, 0]:.4g} [{ec50[i, 1]:.4g}, {ec50[i, 2]:.4g}] true {true_ec50:g}; "
              f"hill {hill[i, 0]:.3f} [{hill_low:.3f}, {hill_high:.3f}] true {true_hill:g}; failed refits {failed}")
        if ec50[i, 1] < conc[0] or ec50[i, 2] > conc[-1]:
            where = f"upper bound {ec50[i, 2]:.3g} µM" if ec50[i, 2] > conc[-1] else f"lower bound {ec50[i, 1]:.3g} µM"
            width = ec50[i, 2] / ec50[i, 1]
            facts = f"The 95% interval's {where} lies outside the measured range {conc[0]:.3g} – {conc[-1]:.3g} µM, and its width is {width:.3g}-fold."
            if width >= BLOWN_UP:
                warnings.append(f"**{name}: the concentration range does not bracket the EC50.** {facts}"
                                "Only one side of the curve is visible, so this EC50 is not determined. Widen the range.")
            else:
                warnings.append(f"**{name}: the interval leaves the measured range.** {facts}"
                                "The EC50 is near the edge of the range and the plateau is not visible. Widening the range narrows it.")

    lines = [f"**95% intervals from {n_boot} residual bootstrap samples** (synthetic data: the true values are known to whoever made it)", "",
             "| Compound | Quantity | True value | Estimate | 95% interval | Width | True value |", "| --- | --- | --- | --- | --- | --- | --- |", *rows, ""]
    lines.append("Every true value fell inside its interval." if missed == 0
                 else f"{missed} true values fell outside their intervals (a 95% interval misses about once in 20).")
    if warnings:
        for warning in warnings:
            lines += ["", warning]
    else:
        lines += ["", f"Both intervals are inside the measured range {conc[0]:.3g} – {conc[-1]:.3g} µM."]
    ctx.display_markdown("\n".join(lines))

    b = ctx.builder()
    names_v = strings(b, names)
    vectors = [b.CreateNumpyVector(v) for v in (
        ec50[:, 0], ec50[:, 1], ec50[:, 2], np.array([plate.Ec50(i) for i in range(n)]),
        hill[:, 0], hill[:, 1], hill[:, 2], np.array([plate.Hill(i) for i in range(n)]),
        np.concatenate(log_samples), np.concatenate(hill_samples))]
    out.Start(b)
    out.AddNames(b, names_v)
    out.AddEc50(b, vectors[0])
    out.AddEc50Low(b, vectors[1])
    out.AddEc50High(b, vectors[2])
    out.AddEc50True(b, vectors[3])
    out.AddHill(b, vectors[4])
    out.AddHillLow(b, vectors[5])
    out.AddHillHigh(b, vectors[6])
    out.AddHillTrue(b, vectors[7])
    out.AddNBoot(b, n_boot)
    out.AddLogEc50Samples(b, vectors[8])
    out.AddHillSamples(b, vectors[9])
    out.AddConcLow(b, conc[0])
    out.AddConcHigh(b, conc[-1])
    ctx.output("intervals", b, out.End(b))
