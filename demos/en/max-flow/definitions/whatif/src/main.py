"""Raise one edge on the cut and one edge off it, and compare what happens."""
import json

import networkx as nx

from inputs.cut.flow.Cut import Cut
from tsugumi import Failure


def solve(edges, raised=None, by=0):
    graph = nx.DiGraph()
    for u, v, c in edges:
        graph.add_edge(u, v, capacity=c + by if (u, v) == raised else c)
    value, (side, _) = nx.minimum_cut(graph, "s", "t")
    return value, [(u, v) for u, v, _ in edges if u in side and v not in side]


def text(pairs):
    return ", ".join(f"{u}→{v}" for u, v in pairs)


def main(inputs, report):
    c = inputs.input("cut", Cut)
    params = inputs.params
    raise_by = int(params.raiseBy)
    wanted = params.edge.decode().strip() if params.edge else ""
    nodes = c.Nodes().decode().split()
    edges = [(nodes[u], nodes[v], int(x)) for u, v, x in zip(c.TailAsNumpy(), c.HeadAsNumpy(), c.CapacityAsNumpy())]
    amounts = {(nodes[u], nodes[v]): int(x) for u, v, x in zip(c.TailAsNumpy(), c.HeadAsNumpy(), c.FlowAsNumpy())}
    side = set(c.SourceSide().decode().split())
    on_cut = [(u, v) for u, v, _ in edges if u in side and v not in side]
    off_cut = [(u, v) for u, v, _ in edges if not (u in side and v not in side)]
    base, base_cut = solve(edges)

    if wanted:
        pair = tuple(wanted.replace("→", "->").split("->"))
        if pair not in amounts:
            raise Failure("output_invalid", f"edge {wanted!r} is not in the network; write it as a1->b2")
        first = pair
    else:
        first = max(on_cut, key=lambda e: solve(edges, e, raise_by)[0])
    capacity = {(u, v): x for u, v, x in edges}
    # Off the cut, a full edge is the most tempting one to widen: it looks like a bottleneck and is not.
    full = [e for e in off_cut if amounts[e] == capacity[e] and e != first]
    second = (full or [e for e in off_cut if e != first])[0] if len(off_cut) > 0 else None
    # A cut edge can gain nothing too, when another minimum cut does not contain it.
    third = next((e for e in on_cut if e != first and solve(edges, e, raise_by)[0] == base), None)

    lines =[f"**Raising a capacity by {raise_by}** (maximum flow was {base})", "", "| Change | On the cut | Flow/capacity | Maximum flow | Gain | New cut |", "| --- | --- | --- | --- | --- | --- |",
             f"| none | | | {base} | | {text(base_cut)} |"]
    rows = []
    for edge in (first, second, third):
        if edge is None:
            continue
        value, cut = solve(edges, edge, raise_by)
        gain = value - base
        lines.append(f"| {edge[0]}→{edge[1]} | {'yes' if edge in on_cut else 'no'} | {amounts[edge]}/{capacity[edge]} | {value} | +{gain} | {text(cut)} |")
        rows.append({"edge": list(edge), "on_cut": edge in on_cut, "value": value, "gain": gain, "cut": [list(e) for e in cut]})
        if edge not in on_cut and gain != 0:
            raise Failure("output_invalid", f"{edge} is off the cut yet the flow rose by {gain}")
        if gain > raise_by:
            raise Failure("output_invalid", f"{edge} gained {gain} from a raise of {raise_by}")

    lines += ["", "**Why**: the maximum flow cannot exceed the capacity of any cut. Raising an edge off the cut leaves the original cut's capacity the same, so "
              "Even an edge running at full capacity does not increase the maximum flow."]
    if rows and rows[0]["on_cut"]:
        g = rows[0]["gain"]
        if g == 0:
            lines.append(f"{first[0]}→{first[1]} is on the cut, but another minimum cut remains, so nothing is gained.")
        elif g < raise_by:
            lines.append(f"{first[0]}→{first[1]} gains only {g} when raised by {raise_by}, because another cut further on gets in the way.")
        else:
            lines.append(f"{first[0]}→{first[1]} gained as much as it was raised. Raise it further and another cut will get in the way sooner or later.")
    if third is not None:
        lines.append(f"{third[0]}→{third[1]} is on the cut, but another minimum cut without this edge remains, so nothing is gained.")
    report.markdown("\n".join(lines))
    report.log(str("result: " + json.dumps({"base": base, "rows": rows})))
