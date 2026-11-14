"""Newton's method from each initial value: the distance to the root it ends at,
iteration by iteration, on a log axis."""
import matplotlib.pyplot as plt
import numpy as np

from tsugumi import Failure

EPS = np.finfo(float).eps


def iterate(coeffs, z0, damping, tolerance, max_iter):
    """The iterates and why they stopped. Below the rounding noise of p(z) a
    further step means nothing, which is what ends a multiple root."""
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


def main(inputs, report):
    p = inputs.problem
    coeffs = p.coeffs.copy()
    roots = p.rootsRe + 1j * p.rootsIm
    multiplicity = p.multiplicity
    params = inputs.params
    start_re, start_im = params.startRe, params.startIm
    if len(start_re) != len(start_im) or len(start_re) == 0:
        raise Failure("output_invalid", "start_re and start_im must have the same length, at least 1")

    fig, ax = plt.subplots(figsize=(8, 5.4))
    ends, statuses, lengths, errors = [], [], [], []
    for z0 in start_re + 1j * start_im:
        path, status = iterate(coeffs, z0, p.damping, p.tolerance, p.maxIter)
        nearest = int(np.argmin(np.abs(roots - path[-1])))
        e = np.abs(np.array(path) - roots[nearest])
        ends.append(nearest)
        statuses.append(status)
        lengths.append(len(e))
        errors.append(e)
        shown = np.where(e > 0, e, np.nan)  # an exact hit has no place on a log axis
        ax.semilogy(shown, marker="o", markersize=3.5, linewidth=1.3,
                    label=f"z₀ = {z0.real:g} {'+' if z0.imag >= 0 else '-'} {abs(z0.imag):g}i: {len(e) - 1} 回")
        report.log(f"z0={z0} status={status} iterations={len(e) - 1} root={roots[nearest]} last_error={e[-1]:.3e}")

    ax.axhline(EPS, color="0.6", linewidth=0.8, linestyle=":")
    ax.text(0.99, EPS * 1.6, "倍精度の丸め", color="0.4", fontsize=9, ha="right", transform=ax.get_yaxis_transform())
    ax.set_xlabel("反復 k")
    ax.set_ylabel("誤差 |z − r|")
    # A line with $...$ is drawn by mathtext, which has no Japanese glyphs: keep them on separate lines.
    ax.set_title(f"$p(z) = {p.label.decode()}$,  $\\lambda = {p.damping:g}$\n初期値ごとの誤差の落ち方", fontsize=12)
    ax.grid(True, which="major", alpha=0.25)
    ax.legend(fontsize=9, loc="upper center", bbox_to_anchor=(0.5, -0.12), ncol=2, frameon=False)
    fig.tight_layout()
    report.figure()
    label = p.label
    status = ' '.join(statuses)
    vec_coeffs = coeffs
    vec_re = np.asarray(start_re, dtype=np.float64)
    vec_im = np.asarray(start_im, dtype=np.float64)
    root_re = np.array([roots[i].real for i in ends])
    root_im = np.array([roots[i].imag for i in ends])
    mult = np.array([multiplicity[i] for i in ends], dtype=np.uint64)
    vec_len = np.array(lengths, dtype=np.uint64)
    vec_err = np.concatenate(errors)
    return {'errors': {
        'label': label,
        'coeffs': vec_coeffs,
        'damping': p.damping,
        'start_re': vec_re,
        'start_im': vec_im,
        'root_re': root_re,
        'root_im': root_im,
        'multiplicity': mult,
        'status': status,
        'lengths': vec_len,
        'errors': vec_err,
    }}
