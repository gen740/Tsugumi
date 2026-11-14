"""The minimum cut by networkx: max-flow = min-cut is asserted, and the network is
drawn with the source side, the sink side and the cut edges marked."""
import tsugumi
import json

import matplotlib.pyplot as plt
import networkx as nx
import numpy as np

from inputs.flow.flow.Flow import Flow
from tsugumi import Failure


def positions(nodes):
    letters = sorted({n[0] for n in nodes if n not in ("s", "t")})
    pos = {"s": (0.0, 0.0), "t": (len(letters) + 1.0, 0.0)}
    for x, letter in enumerate(letters, start=1):
        column = [n for n in nodes if n != "s" and n != "t" and n[0] == letter]
        for j, n in enumerate(column):
            pos[n] = (float(x), (len(column) - 1) / 2 - j)
    return pos


def main(inputs, report):
    f = inputs.input("flow", Flow)
    nodes = f.Nodes().decode().split()
    edges = [(nodes[u], nodes[v], int(c)) for u, v, c in zip(f.TailAsNumpy(), f.HeadAsNumpy(), f.CapacityAsNumpy())]
    amounts = [int(x) for x in f.FlowAsNumpy()]
    graph = nx.DiGraph()
    graph.add_nodes_from(nodes)
    graph.add_weighted_edges_from(edges, weight="capacity")

    capacity, (source_side, _) = nx.minimum_cut(graph, "s", "t")
    crossing = [(u, v, c) for u, v, c in edges if u in source_side and v not in source_side]
    total = sum(c for _, _, c in crossing)
    if total != capacity or capacity != f.Value():
        raise Failure("output_invalid", f"max-flow {f.Value()}, cut capacity {capacity}, crossing edges add up to {total}")
    for (u, v, c), amount in zip(edges, amounts):
        if (u, v, c) in crossing and amount != c:
            raise Failure("output_invalid", f"cut edge {u}->{v} is not saturated: {amount} of {c}")

    pos = positions(nodes)
    fig, ax = plt.subplots(figsize=(9, 6))
    for u, v, c in edges:
        on_cut = (u, v, c) in crossing
        ax.annotate("", xy=pos[v], xytext=pos[u], arrowprops=dict(
            arrowstyle="-|>", color="#c23b4b" if on_cut else "0.78", linewidth=3.2 if on_cut else 1.0, shrinkA=14, shrinkB=14, mutation_scale=12))
        x = pos[u][0] + 0.25 * (pos[v][0] - pos[u][0])  # edges of a layer cross near a third of the way
        y = pos[u][1] + 0.25 * (pos[v][1] - pos[u][1])
        ax.text(x, y, str(c), fontsize=10 if on_cut else 8.5, ha="center", va="center", color="#c23b4b" if on_cut else "0.5",
                bbox=dict(boxstyle="round,pad=0.12", facecolor="white", edgecolor="none", alpha=0.92))
    colors = ["#a9cdea" if n in source_side else "#f4cdb5" for n in nodes]
    nx.draw_networkx_nodes(graph, pos, ax=ax, nodelist=nodes, node_size=520, node_color=colors, edgecolors="0.3")
    nx.draw_networkx_labels(graph, pos, ax=ax, font_size=11)
    ax.set_title(f"最小カット = {capacity} = 最大流（赤の辺 {len(crossing)} 本の容量の和、青は s 側、橙は t 側）", fontsize=11)
    ax.set_axis_off()
    fig.tight_layout()
    report.figure()
    report.log(str("result: " + json.dumps({"capacity": capacity, "source_side": sorted(source_side), "cut_edges": [[u, v, c] for u, v, c in crossing]})))
    names = f.Nodes()
    side = ' '.join(sorted(source_side))
    tail = f.TailAsNumpy()
    head = f.HeadAsNumpy()
    caps = f.CapacityAsNumpy()
    flow = f.FlowAsNumpy()
    _out_value = {}
    _out_value['nodes'] = names
    _out_value['tail'] = tail
    _out_value['head'] = head
    _out_value['capacity'] = caps
    _out_value['flow'] = flow
    _out_value['value'] = f.Value()
    _out_value['source_side'] = side
    return {'cut': _out_value}
