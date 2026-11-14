from tsugumi import Failure


def main(inputs, report):
    found = {
        "C++": list(inputs.cpp.values),
        "Python": list(inputs.python.values),
        "Julia": list(inputs.julia.values),
        "R": list(inputs.r.values),
    }
    a = found["C++"]
    if any(v != a for v in found.values()):
        raise Failure("output_invalid", "sequences differ: " + ", ".join(f"{n} {len(v)} values" for n, v in found.items()))
    report.markdown(
        f"**C++, Python, Julia and R agree**: {len(a)} values, $a_0 = {a[0]}$ reaches $1$ "
        f"after ${len(a) - 1}$ steps (peak $\\max_k a_k = {max(a)}$)."
    )
    return {
        "verdict": {"equal": True, "count": len(a)},
        "sequence": {"values": a},
    }
