"""The same answer by other routes: scipy's maximum_flow (another implementation)
and a brute force over every s-t cut, which also counts the minimum cuts."""
import json

import numpy as np
from scipy.sparse import csr_array
from scipy.sparse.csgraph import maximum_flow

from inputs.cut.flow.Cut import Cut
from tsugumi import Failure

BRUTE_LIMIT = 22  # 2^22 subsets is about a second here; beyond that it is skipped and says so


def brute_force(n, tail, head, capacity):
    """The minimum capacity over all source sides, and the sides that reach it.
    Node 0 is s and node n - 1 is t; each subset of the others is one cut."""
    inner = n - 2
    subsets = np.arange(1 << inner, dtype=np.int64)
    # Bit i of a code says whether node i is on the s side: s is always on it, t never.
    codes = (subsets << 1) | 1
    total = np.zeros(subsets.size, dtype=np.int64)
    for u, v, c in zip(tail, head, capacity):
        total += int(c) * (((codes >> int(u)) & 1) & (1 - ((codes >> int(v)) & 1)))
    best = int(total.min())
    return best, codes[total == best], subsets.size


def main(ctx):
    c = ctx.input("cut", Cut)
    nodes = c.Nodes().decode().split()
    tail, head, capacity = c.TailAsNumpy(), c.HeadAsNumpy(), c.CapacityAsNumpy()
    value = int(c.Value())

    matrix = csr_array((capacity.astype(np.int32), (tail.astype(np.int32), head.astype(np.int32))), shape=(len(nodes), len(nodes)))
    by_scipy = int(maximum_flow(matrix, 0, len(nodes) - 1).flow_value)

    lines = ["**Does another method give the same value?**", "", "| Method | Value |", "| --- | --- |",
             f"| networkx `maximum_flow` (the **flow** Card) | {value} |",
             f"| networkx `minimum_cut` (the **cut** Card) | {value} |",
             f"| scipy `maximum_flow` (another implementation) | {by_scipy} |"]
    if by_scipy != value:
        raise Failure("output_invalid", f"networkx says {value}, scipy says {by_scipy}")

    minimum_cuts = None
    if len(nodes) - 2 <= BRUTE_LIMIT:
        best, codes, tried = brute_force(len(nodes), tail, head, capacity)
        if best != value:
            raise Failure("output_invalid", f"networkx says {value}, the brute force over {tried} cuts says {best}")
        minimum_cuts = [[nodes[i] for i in range(len(nodes)) if (int(code) >> i) & 1] for code in codes]
        lines.append(f"| Brute force over all cuts ({tried}) | {best} |")
        lines += ["", f"**There are {len(minimum_cuts)} minimum cuts** (the nodes on the $s$ side of each):", ""]
        lines += [f"- {{{', '.join(side)}}}" for side in minimum_cuts[:6]]
        if len(minimum_cuts) > 6:
            lines.append(f"- and {len(minimum_cuts) - 6} more")
    else:
        lines += ["", f"Too many nodes ({len(nodes) - 2} inside), so brute force was skipped."]
    ctx.display_markdown("\n".join(lines))
    print("result: " + json.dumps({"networkx": value, "scipy": by_scipy, "minimum_cuts": minimum_cuts}))
