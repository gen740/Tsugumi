"""Builders for test projects placed on disk before tsugumid starts."""

from __future__ import annotations

import json
from pathlib import Path

from materialize import convert_in_place

MSG_FBS = """
namespace demo;
table Msg { text:string; }
table Cond { value:bool; }
table Outputs { msg:Msg (required); cond:Cond; }
root_type Outputs;
"""


def card(id_: str, definition: str | None = None, **settings) -> dict:
    c = {"id": id_}
    if definition:
        c["definition"] = definition
    c.update(settings)
    return c


def conn(a: str, ap: str, b: str, bp: str) -> dict:
    return {"from": {"card": a, "port": ap}, "to": {"card": b, "port": bp}}


def builtin_project(root: Path, text: str = "hello", extra_cards=(), extra_conns=()) -> None:
    """trigger -> input(text) -> logger, with builtin definitions only."""
    root.mkdir(parents=True, exist_ok=True)
    defs = {
        "trigger": {"template": "trigger"},
        "source": {"template": "input", "params": [{"name": "text", "type": "string", "data": "x"},
                                                   {"name": "value", "type": "bool", "data": True}]},
        "log": {"template": "logger"},
        "fork": {"template": "fork"},
        "branch": {"template": "branch"},
        "merge": {"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}]},
    }
    for d, body in defs.items():
        (root / "definitions" / d).mkdir(parents=True, exist_ok=True)
        (root / "definitions" / d / "_card.json").write_text(json.dumps(body))
    (root / "definitions" / "source" / "_outputs.fbs").write_text(MSG_FBS)
    project = {
        "name": "builtin",
        "pkgsPath": "tsugumi:default",
        "databaseConnections": [],
        "canvas": {
            "cards": [card("begin", "trigger"), card("src", "source", params={"text": text}),
                      card("out", "log"), *extra_cards],
            "connections": [conn("begin", "out", "out", "in"), *extra_conns],
        },
    }
    # The input card is a start Card (no inputs); the logger reads its message.
    project["canvas"]["connections"] = [conn("src", "msg", "out", "in"), *extra_conns]
    (root / "tsugumi.json").write_text(json.dumps(project))
    (root / "layout.json").write_text("{}")
    convert_in_place(root)
