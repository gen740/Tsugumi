"""Writes a project described the earlier way (definitions shared by several
Cards, settings overridden on the Card, Card in Card as an inline `canvas`)
in the current form: one definition folder per Card, named by the Card id,
holding every setting, and each Card in Card an external project
(externals/<id>/) linked by definitions/<id>/_link.json.

Tests describe projects compactly the earlier way and write them with this;
the demos were converted with it once."""

from __future__ import annotations

import copy
import json
import shutil
from pathlib import Path

# Settings a Card could override earlier; they now live in its _card.json.
SETTINGS = ("symbol", "description", "maxRuns", "timeoutSeconds", "buildTimeoutSeconds", "recordIO",
            "retainArtifact", "artifactOutput", "filesystem", "sandbox", "reuseWorker", "network",
            "hostFolders", "trigger")
DROPPED = ("name", "canvasEnabled", "loggerDatabaseConnection")


def card_json(definition: dict, card: dict) -> dict:
    """The Card's own _card.json: its definition with the Card's overrides."""
    out = {k: v for k, v in copy.deepcopy(definition).items() if k not in DROPPED}
    if "inputMode" in out:
        out["trigger"] = {"all": "allInputs", "any": "eachInput"}.get(out.pop("inputMode"), "allInputs")
    for k in SETTINGS:
        if k in card:
            out[k] = card[k]
    if "inputMode" in card:
        out["trigger"] = {"all": "allInputs", "any": "eachInput"}[card["inputMode"]]
    for name, value in (card.get("params") or {}).items():
        for p in out.setdefault("params", []):
            if p["name"] == name:
                p["data"] = value
                break
        else:
            raise ValueError(f"card {card['id']}: param {name} is not defined")
    # Files of the definition named by path (an input Card's file) moved with it.
    old = f"definitions/{card.get('definition')}/"
    for p in out.get("params", []):
        if isinstance(p.get("data"), str) and p["data"].startswith(old):
            p["data"] = f"definitions/{card['id']}/" + p["data"][len(old):]
    return out


def scoped(layout: dict, scope: str) -> dict:
    """The part of a layout.json for the Cards inside `scope` (a Card id)."""
    pre = scope + "/"
    out: dict = {}
    for key in ("cards", "displays"):
        part = {k[len(pre):]: v for k, v in (layout.get(key) or {}).items() if k.startswith(pre)}
        if part:
            out[key] = part
    for key in ("notes", "groups"):
        part = []
        for item in layout.get(key) or []:
            s = item.get("scope", "")
            if s == scope or s.startswith(pre):
                item = dict(item)
                item["scope"] = s[len(scope) + 1:] if s != scope else ""
                part.append(item)
        if part:
            out[key] = part
    bars = {("" if k == scope else k[len(pre):]): v for k, v in (layout.get("boundaries") or {}).items()
            if k == scope or k.startswith(pre)}
    if bars:
        out["boundaries"] = bars
    return out


def root_only(layout: dict) -> dict:
    out = {}
    for key in ("cards", "displays"):
        part = {k: v for k, v in (layout.get(key) or {}).items() if "/" not in k}
        if part:
            out[key] = part
    for key in ("notes", "groups"):
        part = [i for i in layout.get(key) or [] if i.get("scope", "") == ""]
        if part:
            out[key] = part
    if "" in (layout.get("boundaries") or {}):
        out["boundaries"] = {"": layout["boundaries"][""]}
    return out


def write_project(out: Path, project: dict, definitions: Path, layout: dict | None = None,
                  graphs: Path | None = None) -> None:
    """Writes `project` (the earlier form) to `out`, taking definition folders
    from `definitions` (the earlier definitions/, read only)."""
    layout = layout or {}
    out.mkdir(parents=True, exist_ok=True)
    defs = out / "definitions"
    defs.mkdir(exist_ok=True)
    canvas = project["canvas"]
    for c in canvas["cards"]:
        cid = c["id"]
        target = defs / cid
        if target.exists():
            shutil.rmtree(target)
        inner = c.get("canvas")
        if inner is None and c.get("project"):
            inner = json.loads(((graphs or out) / c["project"]).read_text())
        if inner is not None:
            ext = {"name": c.get("name", cid), "pkgsPath": project.get("pkgsPath", "tsugumi:default"),
                   "databaseConnections": [], "canvas": inner}
            if c.get("boundary"):
                ext["boundary"] = c["boundary"]
            write_project(out / "externals" / cid, ext, definitions, scoped(layout, cid), graphs)
            target.mkdir(parents=True)
            (target / "_link.json").write_text(json.dumps({"external": cid}, indent=2) + "\n")
            continue
        name = c.get("definition")
        src = definitions / name if name else None
        if src and src.is_dir():
            shutil.copytree(src, target)
            body = json.loads((src / "_card.json").read_text())
        else:
            target.mkdir(parents=True)
            body = {"template": "draft"}
        (target / "_card.json").write_text(json.dumps(card_json(body, c), indent=2, ensure_ascii=False) + "\n")
    doc = {k: v for k, v in project.items() if k != "canvas"}
    doc["canvas"] = {"cards": [{"id": c["id"]} for c in canvas["cards"]], "connections": canvas["connections"]}
    (out / "tsugumi.json").write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n")
    (out / "layout.json").write_text(json.dumps(root_only(layout), indent=2, ensure_ascii=False) + "\n")


def convert_in_place(root: Path) -> None:
    """Rewrites the project at `root` (written the earlier way) in place."""
    project = json.loads((root / "tsugumi.json").read_text())
    layout_file = root / "layout.json"
    layout = json.loads(layout_file.read_text()) if layout_file.exists() else {}
    old = root.parent / (root.name + ".definitions-earlier")
    if old.exists():
        shutil.rmtree(old)
    if (root / "definitions").exists():
        (root / "definitions").rename(old)
    old.mkdir(exist_ok=True)
    try:
        write_project(root, project, old, layout, graphs=root)
    finally:
        shutil.rmtree(old)
    shutil.rmtree(root / "graphs", ignore_errors=True)
