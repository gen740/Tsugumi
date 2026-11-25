"""Raise one edge on the cut and one edge off it, and compare what happens."""
from card import Artifact, Input, Output, params
import json
from pathlib import Path

import networkx as nx

from tsugumi import Failure, Markdown, send


def solve(edges, raised=None, by=0):
    graph = nx.DiGraph()
    for u, v, c in edges:
        graph.add_edge(u, v, capacity=c + by if (u, v) == raised else c)
    value, (side, _) = nx.minimum_cut(graph, "s", "t")
    return value, [(u, v) for u, v, _ in edges if u in side and v not in side]


def text(pairs):
    return "、".join(f"{u}→{v}" for u, v in pairs)


def run(inputs: Input) -> Output:
    c = inputs.require("cut")
    raise_by = int(params.raise_by)
    wanted = params.edge.strip()
    nodes = c.nodes.decode().split()
    edges = [(nodes[u], nodes[v], int(x)) for u, v, x in zip(c.tail, c.head, c.capacity)]
    amounts = {(nodes[u], nodes[v]): int(x) for u, v, x in zip(c.tail, c.head, c.flow)}
    side = set(c.sourceSide.decode().split())
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
    second = next(iter(full or [e for e in off_cut if e != first]), None)
    # A cut edge can gain nothing too, when another minimum cut does not contain it.
    third = next((e for e in on_cut if e != first and solve(edges, e, raise_by)[0] == base), None)

    lines =[f"**容量を {raise_by} 増やすと**（元の最大流 {base}）", "", "| 変更 | カット上か | 流量/容量 | 最大流 | 増分 | 新しいカット |", "| --- | --- | --- | --- | --- | --- |",
             f"| なし | | | {base} | | {text(base_cut)} |"]
    rows = []
    for edge in (first, second, third):
        if edge is None:
            continue
        value, cut = solve(edges, edge, raise_by)
        gain = value - base
        lines.append(f"| {edge[0]}→{edge[1]} | {'はい' if edge in on_cut else 'いいえ'} | {amounts[edge]}/{capacity[edge]} | {value} | +{gain} | {text(cut)} |")
        rows.append({"edge": list(edge), "on_cut": edge in on_cut, "value": value, "gain": gain, "cut": [list(e) for e in cut]})
        if edge not in on_cut and gain != 0:
            raise Failure("output_invalid", f"{edge} is off the cut yet the flow rose by {gain}")
        if gain > raise_by:
            raise Failure("output_invalid", f"{edge} gained {gain} from a raise of {raise_by}")

    lines += ["", "**なぜか**: 最大流はどのカットの容量も超えられません。カット外の辺を増やしても元のカットの容量は同じなので、"
              "容量いっぱいに流れている辺でも最大流は増えません。"]
    if rows and rows[0]["on_cut"]:
        g = rows[0]["gain"]
        if g == 0:
            lines.append(f"{first[0]}→{first[1]} はカットの上にありますが、別の最小カットが残っているので増えません。")
        elif g < raise_by:
            lines.append(f"{first[0]}→{first[1]} は {raise_by} 増やしても {g} しか増えません。その先で別のカットが詰まるからです。")
        else:
            lines.append(f"{first[0]}→{first[1]} は増やした分だけ増えました。もっと増やせば、いずれ別のカットが詰まります。")
    if third is not None:
        lines.append(f"{third[0]}→{third[1]} はカットの上にありますが、この辺を含まない別の最小カットが残っているので増えません。")
    artifact, directory = Artifact.dir()
    markdown = "\n".join(lines)
    (Path(directory) / "summary.md").write_text(markdown)
    send(Markdown(markdown))
    (Path(directory) / "result.json").write_text(json.dumps({"base": base, "rows": rows}, indent=2))
    return Output(result=artifact)
