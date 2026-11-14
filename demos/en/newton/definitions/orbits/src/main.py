"""Newton's method from each initial value: the distance to the root it ends at,
iteration by iteration, on a log axis."""
import tsugumi
import matplotlib.pyplot as plt
import numpy as np

from inputs.problem.newton.Problem import Problem
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
    p = inputs.input("problem", Problem)
    coeffs = p.CoeffsAsNumpy().copy()
    roots = p.RootsReAsNumpy() + 1j * p.RootsImAsNumpy()
    multiplicity = p.MultiplicityAsNumpy()
    params = inputs.params
    start_re, start_im = params.startReAsNumpy, params.startImAsNumpy
    if len(start_re) != len(start_im) or len(start_re) == 0:
        raise Failure("output_invalid", "start_re and start_im must have the same length, at least 1")

    fig, ax = plt.subplots(figsize=(8, 5.4))
    ends, statuses, lengths, errors = [], [], [], []
    for z0 in start_re + 1j * start_im:
        path, status = iterate(coeffs, z0, p.Damping(), p.Tolerance(), p.MaxIter())
        nearest = int(np.argmin(np.abs(roots - path[-1])))
        e = np.abs(np.array(path) - roots[nearest])
        ends.append(nearest)
        statuses.append(status)
        lengths.append(len(e))
        errors.append(e)
        shown = np.where(e > 0, e, np.nan)  # an exact hit has no place on a log axis
        ax.semilogy(shown, marker="o", markersize=3.5, linewidth=1.3,
                    label=f"z₀ = {z0.real:g} {'+' if z0.imag >= 0 else '-'} {abs(z0.imag):g}i: {len(e) - 1} iterations")
        report.log(str(f"z0={z0} status={status} iterations={len(e) - 1} root={roots[nearest]} last_error={e[-1]:.3e}"))

    ax.axhline(EPS, color="0.6", linewidth=0.8, linestyle=":")
    ax.text(0.99, EPS * 1.6, "double-precision rounding", color="0.4", fontsize=9, ha="right", transform=ax.get_yaxis_transform())
    ax.set_xlabel("Iteration k")
    ax.set_ylabel("Error |z − r|")
    # A line with $...$ is drawn by mathtext, which has no Japanese glyphs: keep them on separate lines.
    ax.set_title(f"$p(z) = {p.Label().decode()}$,  $\\lambda = {p.Damping():g}$\nHow the error falls for each starting point", fontsize=12)
    ax.grid(True, which="major", alpha=0.25)
    ax.legend(fontsize=9, loc="upper center", bbox_to_anchor=(0.5, -0.12), ncol=2, frameon=False)
    fig.tight_layout()
    report.figure()
    label = p.Label()
    status = ' '.join(statuses)
    vec_coeffs = coeffs
    vec_re = np.asarray(start_re, dtype=np.float64)
    vec_im = np.asarray(start_im, dtype=np.float64)
    root_re = np.array([roots[i].real for i in ends])
    root_im = np.array([roots[i].imag for i in ends])
    mult = np.array([multiplicity[i] for i in ends], dtype=np.uint64)
    vec_len = np.array(lengths, dtype=np.uint64)
    vec_err = np.concatenate(errors)
    _out_value = {}
    _out_value['label'] = label
    _out_value['coeffs'] = vec_coeffs
    _out_value['damping'] = p.Damping()
    _out_value['start_re'] = vec_re
    _out_value['start_im'] = vec_im
    _out_value['root_re'] = root_re
    _out_value['root_im'] = root_im
    _out_value['multiplicity'] = mult
    _out_value['status'] = status
    _out_value['lengths'] = vec_len
    _out_value['errors'] = vec_err
    return {'errors': _out_value}
