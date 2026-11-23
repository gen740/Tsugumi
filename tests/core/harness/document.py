"""Build fixtures using the current project's FlatBuffers document schemas."""

import json
import os
from pathlib import Path
import subprocess
import tempfile

from Tsugumi.Layout.Layout import Layout

PROTOCOL = Path(__file__).resolve().parents[3] / "protocol"


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


def set_param(body, name, value):
    body.setdefault("params", {})[name] = value
