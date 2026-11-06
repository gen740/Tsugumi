"""The Card graphs of the bundled demos, as the 整列 tests take them.

Port names come from a real tsugumid (they are what `resolve` says), so the
tests in Web and Swift lay out the graphs the canvas really has without needing
a tsugumid themselves. Writes tests/fixtures/arrange-demos.json:

    python3 tests/python/arrange_cases.py --write
"""

from __future__ import annotations

import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from conftest import ROOT, Daemon  # noqa: E402
from tsugumi_client import Client, text  # noqa: E402

FIXTURE = ROOT / "tests" / "fixtures" / "arrange-demos.json"
KINDS = {"trigger", "input", "fork", "branch", "merge", "logger", "draft", "subgraph", "code"}
INLINE_PARAMS = {"int64", "uint64", "double", "bool", "string"}


def read(path: Path):
    return json.loads(path.read_text()) if path.exists() else None


def ports(demo: Path) -> dict:
    """What tsugumid resolves each Card's ports to, by Card path."""
    tmp = Path(tempfile.mkdtemp(prefix="tsga-")) / demo.name
    shutil.copytree(demo, tmp)
    daemon = Daemon(tmp).start(timeout=60)
    try:
        c = Client(unix=daemon.unix)
        c.hello()
        c.call("ProjectOpenRequest", timeout=120)
        snap = c.call("ProjectInspectRequest", timeout=120)
        c.close()
        return {"/".join(text(s) for s in r.card.segments): {"inputs": [text(p.name) for p in r.inputs],
                                                              "outputs": [text(p.name) for p in r.outputs]}
                for r in snap.resolution}
    finally:
        daemon.stop()
        shutil.rmtree(tmp.parent, ignore_errors=True)


def card_node(folder: Path, cid: str, resolved: dict, frames: dict) -> dict:
    definition = folder / "definitions" / cid
    body = read(definition / "_card.json") or {}
    meta = read(definition / "_meta.json") or {}
    if (definition / "_link.json").exists():
        kind = "subgraph"
    elif body.get("template") in KINDS:
        kind = body["template"]
    else:
        kind = "code" if "language" in body else "draft"
    summary = body.get("description") or meta.get("description")
    if kind == "draft":
        summary = meta.get("content") or summary
    params = 0
    if kind == "input":
        params = sum(1 for p in body.get("params", []) if p.get("name") and p.get("type") in INLINE_PARAMS)
    frame = frames.get(cid)
    return {"id": cid, "kind": kind, "summary": bool(summary), "params": params,
            "inputs": resolved.get("inputs", []), "outputs": resolved.get("outputs", []),
            "display": bool(frame) and frame.get("hidden") is not True}


def scope_cases(folder: Path, scope: list, name: str, flow: str, resolved: dict) -> list:
    doc = read(folder / "tsugumi.json")
    layout = read(folder / "layout.json") or {}
    frames = layout.get("displays", {})
    canvas = doc["canvas"]
    prefix = "/".join(scope) + "/" if scope else ""
    cards = [card_node(folder, c["id"], resolved.get(prefix + c["id"], {}), frames) for c in canvas["cards"]]
    case = {
        "name": name, "flow": flow, "cards": cards,
        "wires": [{"from": c["from"]["card"], "fromPort": c["from"]["port"], "to": c["to"]["card"], "toPort": c["to"]["port"]}
                  for c in canvas["connections"]],
        "frames": [{"id": c["id"], "width": frames[c["id"]].get("width", 320), "height": frames[c["id"]].get("height", 170)}
                   for c in cards if c["display"]],
        "notes": [{"id": n["id"], "width": n.get("width", 240), "height": n.get("height", 120)}
                  for n in layout.get("notes", []) if not n.get("scope")],
    }
    boundary = doc.get("boundary")
    if scope and boundary:
        side = lambda s: [{"card": v["card"], "port": v["port"]} for v in boundary.get(s, {}).values()]
        case["boundary"] = {"inputs": side("inputs"), "outputs": side("outputs")}
    out = [case]
    for c in canvas["cards"]:
        link = read(folder / "definitions" / c["id"] / "_link.json")
        if link and (folder / "externals" / link["external"] / "tsugumi.json").exists():
            out += scope_cases(folder / "externals" / link["external"], [*scope, c["id"]], f"{name}/{c['id']}", flow, resolved)
    return out


def demo_flow(demo: Path) -> str:
    layout = read(demo / "layout.json") or {}
    if layout.get("flow"):
        return layout["flow"]
    across = down = 0.0
    at = layout.get("cards", {})
    for c in read(demo / "tsugumi.json")["canvas"]["connections"]:
        a, b = at.get(c["from"]["card"], {}), at.get(c["to"]["card"], {})
        across += abs(b.get("x", 0) - a.get("x", 0))
        down += abs(b.get("y", 0) - a.get("y", 0))
    return "leftToRight" if across > down else "topToBottom"


def all_cases() -> list:
    out = []
    for demo in sorted((ROOT / "demos").iterdir()):
        if (demo / "tsugumi.json").exists():
            out += scope_cases(demo, [], demo.name, demo_flow(demo), ports(demo))
    return out


if __name__ == "__main__":
    cases = all_cases()
    if "--write" in sys.argv:
        FIXTURE.write_text(json.dumps(cases, ensure_ascii=False, separators=(",", ":")).replace('},{"name"', '},\n{"name"') + "\n")
    print(len(cases), "scopes")
