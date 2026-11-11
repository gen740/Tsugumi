"""Every edge raised in turn: the gain in max flow, edge by edge. An edge off the
cut must gain nothing, since the cut it is off of keeps its capacity."""
import json

import matplotlib.pyplot as plt
import networkx as nx

from inputs.cut.flow.Cut import Cut
from tsugumi import Failure


def max_flow(edges, raised, by):
    graph = nx.DiGraph()
    for u, v, c in edges:
        graph.add_edge(u, v, capacity=c + by if (u, v) == raised else c)
    return nx.maximum_flow_value(graph, "s", "t")


def main(ctx):
    c = ctx.input("cut", Cut)
    raise_by = int(ctx.params().RaiseBy())
    nodes = c.Nodes().decode().split()
    edges = [(nodes[u], nodes[v], int(x)) for u, v, x in zip(c.TailAsNumpy(), c.HeadAsNumpy(), c.CapacityAsNumpy())]
    side = set(c.SourceSide().decode().split())
    base = int(c.Value())

    rows = []
    for u, v, capacity in edges:
        gain = max_flow(edges, (u, v), raise_by) - base
        on_cut = u in side and v not in side
        if not on_cut and gain != 0:
            raise Failure("output_invalid", f"{u}->{v} is off the cut yet the flow rose by {gain}")
        rows.append({"edge": [u, v], "capacity": capacity, "on_cut": on_cut, "gain": gain})

    fig, ax = plt.subplots(figsize=(10, 4.8))
    x = range(len(rows))
    ax.bar(x, [r["gain"] for r in rows], color=["#c23b4b" if r["on_cut"] else "0.7" for r in rows])
    for i, r in zip(x, rows):  # the number also marks the zero-height bars: each edge was tried
        ax.text(i, r["gain"] + raise_by * 0.012, str(r["gain"]), ha="center", va="bottom", fontsize=9, color="#c23b4b" if r["on_cut"] else "0.4")
    ax.axhline(raise_by, color="0.4", linewidth=0.9, linestyle="--")
    ax.text(len(rows) - 0.5, raise_by, f" raised by {raise_by}", va="bottom", ha="right", fontsize=9, color="0.3")
    ax.set_xticks(list(x), [f"{r['edge'][0]}→{r['edge'][1]}" for r in rows], rotation=60, ha="right", fontsize=9)
    for tick, r in zip(ax.get_xticklabels(), rows):
        if r["on_cut"]:
            tick.set_color("#c23b4b")
    ax.set_ylabel("Increase in maximum flow")
    ax.set_ylim(0, raise_by * 1.15)
    gainers = sum(r["gain"] > 0 for r in rows)
    cut_edges = sum(r["on_cut"] for r in rows)
    ax.set_title(f"Raising each edge's capacity by {raise_by} (maximum flow was {base}): {gainers} of {len(rows)} edges increase it; red are the cut's {cut_edges}", fontsize=11)
    ax.grid(True, axis="y", alpha=0.25)
    fig.tight_layout()
    plt.show()
    print("result: " + json.dumps({"base": base, "rows": rows}))
