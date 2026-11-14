"""A synthetic layered network from a seed: s, then `layers` layers of `width`
nodes, then t. s reaches all of the first layer and all of the last reaches t."""
import json
import string

import numpy as np

from tsugumi import Failure


def main(inputs, report):
    s = inputs.settings
    layers, width, probability, top = int(s.layers), int(s.width), s.edgeProbability, int(s.maxCapacity)
    if not 1 <= layers <= 26 or width < 1 or top < 1:
        raise Failure("output_invalid", "layers must be 1 to 26, width and max_capacity at least 1")
    rng = np.random.default_rng(s.seed)

    grid = [[f"{string.ascii_lowercase[i]}{j + 1}" for j in range(width)] for i in range(layers)]
    nodes = ["s"] + [n for layer in grid for n in layer] + ["t"]
    index = {n: i for i, n in enumerate(nodes)}

    pairs = [("s", n) for n in grid[0]]
    for here, there in zip(grid, grid[1:]):
        chosen = {(u, v) for u in here for v in there if rng.random() < probability}
        for u in here:  # every node keeps a way out and a way in
            if not any(a == u for a, _ in chosen):
                chosen.add((u, there[rng.integers(width)]))
        for v in there:
            if not any(b == v for _, b in chosen):
                chosen.add((here[rng.integers(width)], v))
        pairs += sorted(chosen)
    pairs += [(n, "t") for n in grid[-1]]
    capacity = rng.integers(1, top + 1, size=len(pairs))

    out_of_s = sum(int(c) for (u, _), c in zip(pairs, capacity) if u == "s")
    into_t = sum(int(c) for (_, v), c in zip(pairs, capacity) if v == "t")
    report.markdown(
        f"**架空のネットワーク**（seed {s.seed} から生成）\n\n"
        f"- 節点 {len(nodes)}、辺 {len(pairs)}\n"
        f"- 容量 {capacity.min()} から {capacity.max()}\n"
        f"- $s$ から出る総容量 {out_of_s}、$t$ に入る総容量 {into_t}"
    )
    report.log("result: " + json.dumps({"nodes": nodes, "edges": [[u, v, int(c)] for (u, v), c in zip(pairs, capacity)]}))
    names = ' '.join(nodes)
    tail = np.array([index[u] for (u, _) in pairs], dtype=np.uint64)
    head = np.array([index[v] for (_, v) in pairs], dtype=np.uint64)
    caps = capacity.astype(np.uint64)
    return {'network': {
        'nodes': names,
        'tail': tail,
        'head': head,
        'capacity': caps,
    }}
