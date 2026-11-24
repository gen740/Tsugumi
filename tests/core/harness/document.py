"""Build fixtures using the current project's FlatBuffers document schemas."""

import json
import os
import platform
import sys
from pathlib import Path
import subprocess
import tempfile

from Tsugumi.Layout.Layout import Layout
from tsugumi_client import change, new_id, path, text

PROTOCOL = Path(__file__).resolve().parents[3] / "protocol"


MACHINE = {"arm64": "aarch64", "amd64": "x86_64"}.get(
    platform.machine().lower(), platform.machine().lower())
NATIVE_SYSTEM = f"{MACHINE}-{'darwin' if sys.platform == 'darwin' else 'linux'}"

SYMBOLS = {
    "code": "chevron.left.forwardslash.chevron.right", "input": "slider.horizontal.3",
    "trigger": "play.circle", "merge": "arrow.triangle.merge",
    "collect": "line.3.horizontal.decrease", "draft": "pencil.and.outline",
    "subgraph": "link",
}


def project_document(**fields):
    return {
        "formatVersion": 1, "name": "test", "nixPath": [],
        "trackedPaths": [], "agentExclude": [], "recordLogs": True, "recordIO": False,
        "logLimitBytes": 134217728, "outputLimitBytes": 68719476736,
        "outputLimitEntries": 1000, "canvas": {"cards": [], "connections": []},
        **fields,
    }


def card_document(fields):
    kind = fields["type"]
    body = {"type": kind, "symbol": SYMBOLS[kind]}
    if kind in {"code", "input", "merge", "collect"}:
        body.update(maxRuns=1, recordIO="project")
    if kind == "merge":
        body["inputs"] = [{"name": "a"}, {"name": "b"}]
    if kind in {"code", "input"}:
        body.update(inputs=[], reuseWorker=False, timeoutSeconds=0, buildTimeoutSeconds=0,
                    recordLogs="project", systems=[NATIVE_SYSTEM], workerCapabilities=[])
    body.update(fields)
    if kind in {"code", "input"}:
        sandbox = body.get("sandbox", {})
        body["sandbox"] = {
            "filesystem": True, "pid": True, "ipc": True, "identity": True,
            **sandbox, "limits": {"cpu": 0, "memoryMiB": 0, **sandbox.get("limits", {})},
        }
        body["grants"] = {"network": False, "gui": False, **body.get("grants", {})}
    return body


def card_readme(title, description="", body=""):
    return f"---\ntitle: {title}\ndescription: {description}\n---\n" + (
        f"\n{body}" if body else "")


def build_buffer(schema, root_type, value):
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "fixture.json"
        source.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")
        subprocess.run(
            [os.environ.get("TSUGUMI_FLATC", "flatc"), "--binary",
             "--strict-json", "--no-warnings", "-I", PROTOCOL, "--root-type", root_type,
             "-o", directory, PROTOCOL / f"{schema}.fbs", source],
            check=True, capture_output=True,
        )
        return source.with_suffix(".bin").read_bytes()


def layout_bytes(view):
    layout = dict(view)
    for field, key in (("cards", "key"), ("boundaries", "scope")):
        if field in layout:
            layout[field] = [{key: name, **value} for name, value in layout[field].items()]
    if "notes" in layout:
        layout["notes"] = [{k: v for k, v in note.items() if k != "text"} for note in layout["notes"]]
    return build_buffer("layout_document", "Tsugumi.Layout.Layout", layout)


def write_layout(root, layout):
    (root / "layout.bin").write_bytes(layout_bytes(layout))
    for note in layout.get("notes", []):
        file = root / "notes" / f"{note['id']}.md"
        file.parent.mkdir(exist_ok=True)
        file.write_text(note.get("text", ""), encoding="utf-8")


def positions(buffer):
    layout = Layout.GetRootAs(bytes(buffer))
    cards = [layout.Cards(i) for i in range(layout.CardsLength())]
    return {card.Key().decode(): {"x": card.X(), "y": card.Y()} for card in cards}


def nix_literal(value):
    if isinstance(value, str):
        escaped = value.replace("\\", "\\\\").replace('"', '\\"').replace("$" + "{", "\\$" + "{")
        return '"' + escaped + '"'
    if isinstance(value, list):
        items = [nix_literal(item) for item in value]
        return "[ " + " ".join(f"({item})" if item.startswith("-") else item
                               for item in items) + " ]"
    literal = json.dumps(value)
    mantissa, exponent, power = literal.partition("e")
    if type(value) is float and exponent and "." not in mantissa:
        return mantissa + ".0e" + power
    return literal


def params_nix(values):
    return "{\n" + "".join(f"  {nix_literal(name)} = {nix_literal(value)};\n"
                           for name, value in sorted(values.items())) + "}\n"


def set_param(client, card, name, value):
    rev = client.call("ProjectInspectRequest").rev
    rewritten = client.call("RewriteParamLiteralRequest", card=path(*card.split("/")),
                            name=name, literal=nix_literal(value), atRev=rev)
    client.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="ui:test",
                changes=[change("PutSource", path=text(rewritten.path),
                                baseHash=text(rewritten.baseHash), bytes=rewritten.text)])
