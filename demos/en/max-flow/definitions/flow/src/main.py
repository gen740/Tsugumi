"""The maximum flow by networkx, checked against the capacities and conservation,
drawn on the network: each edge shows flow / capacity, thicker for more flow."""
import json

import matplotlib.pyplot as plt
import networkx as nx
import numpy as np

from inputs.network.flow.Network import Network
import outputs.flow.flow.Flow as out
from tsugumi import Failure


def positions(nodes):
    """s on the left, t on the right, one column per layer letter."""
    letters = sorted({n[0] for n in nodes if n not in ("s", "t")})
    pos = {"s": (0.0, 0.0), "t": (len(letters) + 1.0, 0.0)}
    for x, letter in enumerate(letters, start=1):
        column = [n for n in nodes if n != "s" and n != "t" and n[0] == letter]
        for j, n in enumerate(column):
            pos[n] = (float(x), (len(column) - 1) / 2 - j)
    return pos


def main(ctx):
    net = ctx.input("network", Network)
    nodes = net.Nodes().decode().split()
    edges = [(nodes[u], nodes[v], int(c)) for u, v, c in zip(net.TailAsNumpy(), net.HeadAsNumpy(), net.CapacityAsNumpy())]
    graph = nx.DiGraph()
    graph.add_nodes_from(nodes)
    graph.add_weighted_edges_from(edges, weight="capacity")

    value, flow = nx.maximum_flow(graph, "s", "t")
    amounts = [flow[u][v] for u, v, _ in edges]
    for (u, v, c), f in zip(edges, amounts):
        if not 0 <= f <= c:
            raise Failure("output_invalid", f"{u}->{v} carries {f} over its capacity {c}")
    for n in nodes:
        balance = sum(flow[n].values()) - sum(flow[m].get(n, 0) for m in nodes)
        expected = value if n == "s" else (-value if n == "t" else 0)
        if balance != expected:
            raise Failure("output_invalid", f"conservation fails at {n}: {balance}")

    pos = positions(nodes)
    fig, ax = plt.subplots(figsize=(9, 6))
    top = max(max(amounts), 1)
    full = [f == c for (_, _, c), f in zip(edges, amounts)]
    for (u, v, c), f, saturated in zip(edges, amounts, full):
        color = "#d9622b" if saturated else ("#2a7ab9" if f > 0 else "0.75")
        ax.annotate("", xy=pos[v], xytext=pos[u], arrowprops=dict(
            arrowstyle="-|>", color=color, linewidth=0.8 + 4 * f / top, shrinkA=14, shrinkB=14, mutation_scale=11))
        x = pos[u][0] + 0.25 * (pos[v][0] - pos[u][0])  # edges of a layer cross near a third of the way
        y = pos[u][1] + 0.25 * (pos[v][1] - pos[u][1])
        ax.text(x, y, f"{f}/{c}", fontsize=9, ha="center", va="center", color=color if f > 0 else "0.45",
                bbox=dict(boxstyle="round,pad=0.12", facecolor="white", edgecolor="none", alpha=0.92))
    nx.draw_networkx_nodes(graph, pos, ax=ax, node_size=520, node_color="#f2f2f2", edgecolors="0.3")
    nx.draw_networkx_labels(graph, pos, ax=ax, font_size=11)
    ax.set_title(f"Maximum flow = {value} (each edge is flow/capacity; orange at full capacity, grey with no flow. A made-up network)", fontsize=11)
    ax.set_axis_off()
    fig.tight_layout()
    plt.show()
    print("result: " + json.dumps({"value": value, "flow": [[u, v, f] for (u, v, _), f in zip(edges, amounts)]}))

    b = ctx.builder()
    names = b.CreateString(net.Nodes())
    tail = b.CreateNumpyVector(net.TailAsNumpy())
    head = b.CreateNumpyVector(net.HeadAsNumpy())
    caps = b.CreateNumpyVector(net.CapacityAsNumpy())
    vec = b.CreateNumpyVector(np.array(amounts, dtype=np.uint64))
    out.Start(b)
    out.AddNodes(b, names)
    out.AddTail(b, tail)
    out.AddHead(b, head)
    out.AddCapacity(b, caps)
    out.AddFlow(b, vec)
    out.AddValue(b, value)
    ctx.output("flow", b, out.End(b))
