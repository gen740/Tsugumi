"""Test projects written on disk in their current form (one definition
folder per Card, Card in Card as an external project), and running them."""

from __future__ import annotations

import copy
import json
from pathlib import Path

from tsugumi_client import RpcFailure, new_id, path, text
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.RunEventValue import RunEventValue

MSG_FBS = """
namespace demo;
table Msg { text:string; }
table Cond { value:bool; }
table Outputs { msg:Msg (required); cond:Cond; }
root_type Outputs;
"""

FILE_FBS = """
namespace files;
table Doc { value:string; }
table Outputs { doc:Doc (required, tsugumi: "file"); }
root_type Outputs;
attribute "tsugumi";
"""

# A zero-branch fork consumes its required input without emitting another value.
SINK = {"template": "fork", "params": [{"name": "branches", "type": "string[]", "data": []}]}

ENDED = (RunState.succeeded, RunState.failed, RunState.cancelled)


def put(p: Path, data: str) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(data)


def card(id_: str, definition: str | None = None, **settings) -> dict:
    """A Card made from `definition` (a key of the defs given to write), with
    settings of its own; params={"name": value} set those params' values."""
    c = {"id": id_}
    if definition:
        c["definition"] = definition
    c.update(settings)
    return c


def card_in_card(id_: str, cards: list, conns: list, boundary: dict) -> dict:
    """A Card in Card: written as the external project externals/<id>/."""
    return {"id": id_, "_inner": {"cards": cards, "connections": conns, "boundary": boundary}}


def conn(a: str, ap: str, b: str, bp: str) -> dict:
    return {"from": {"card": a, "port": ap}, "to": {"card": b, "port": bp}}


def write(root: Path, defs: dict, cards: list, conns: list, files: dict | None = None, name: str = "test") -> None:
    """Writes a project. `defs` are the bodies of _card.json by name; a body
    may carry "_outputs" (its _outputs.fbs) and "_nix" (its default.nix).
    `files` are written as they are, except those under
    definitions/<a def>/, which every Card made from it gets."""
    files = files or {}
    root.mkdir(parents=True, exist_ok=True)
    for c in cards:
        cid = c["id"]
        folder = root / "definitions" / cid
        if "_inner" in c:
            inner = c["_inner"]
            write(root / "externals" / cid, defs, inner["cards"], inner["connections"],
                  {k: v for k, v in files.items() if k.startswith("definitions/")}, name=cid)
            doc = json.loads((root / "externals" / cid / "tsugumi.json").read_text())
            doc["boundary"] = inner["boundary"]
            put(root / "externals" / cid / "tsugumi.json", json.dumps(doc))
            put(folder / "_link.json", json.dumps({"external": cid}))
            continue
        d = c.get("definition")
        body = copy.deepcopy(defs[d]) if d else {"template": "draft"}
        outputs, nix = body.pop("_outputs", None), body.pop("_nix", None)
        for k, v in c.items():
            if k not in ("id", "definition", "params"):
                body[k] = v
        for pname, value in (c.get("params") or {}).items():
            match = [p for p in body.get("params", []) if p["name"] == pname]
            if not match:
                raise ValueError(f"card {cid}: param {pname} is not defined")
            match[0]["data"] = value
        # Card-local file parameters need no rewriting when a definition is reused.
        mine, theirs = f"definitions/{cid}/", f"definitions/{d}/"
        put(folder / "_card.json", json.dumps(body))
        if outputs:
            put(folder / "_outputs.fbs", outputs)
        if nix:
            put(folder / "default.nix", nix)
        for rel, data in files.items():
            if d and rel.startswith(theirs):
                put(root / (mine + rel[len(theirs):]), data)
    defs_names = {f"definitions/{d}/" for d in defs}
    for rel, data in files.items():
        if not any(rel.startswith(p) for p in defs_names):
            put(root / rel, data)
    put(root / "tsugumi.json", json.dumps({
        "name": name, "pkgsPath": "tsugumi:default",
        "canvas": {"cards": [{"id": c["id"]} for c in cards], "connections": conns}}))
    put(root / "layout.json", "{}")


def builtin_project(root: Path, text: str = "hello", extra_cards=(), extra_conns=()) -> None:
    """An input Card wired to a sink, with a trigger for whole-workflow runs."""
    defs = {
        "trigger": {"template": "trigger"},
        "source": {"template": "input", "params": [{"name": "text", "type": "string", "data": "x"},
                                                   {"name": "value", "type": "bool", "data": True}],
                   "_outputs": MSG_FBS},
        "sink": SINK,
        "fork": {"template": "fork"},
        "branch": {"template": "branch"},
        "merge": {"template": "merge", "inputs": [{"name": "a"}, {"name": "b"}]},
    }
    # The input Card is a start Card (no inputs).
    write(root, defs, [card("start", "trigger"), card("src", "source", params={"text": text}), card("out", "sink"),
                       *extra_cards],
          [conn("src", "msg", "out", "in"), *extra_conns], name="builtin")


def start_run(c, card_path, propagate: bool = True, concurrency: int | None = 4) -> str:
    """Starts a run from a Card ("id", "outer/inner" or a list of ids); its run_id."""
    c.ensure_subscription()
    segments = card_path.split("/") if isinstance(card_path, str) else list(card_path)
    rev = c.call("ProjectInspectRequest").rev
    extra = {"maxConcurrency": concurrency} if concurrency else {}
    acc = c.call("RunRequest", baseRev=rev, opId=new_id(), origin="test", card=path(*segments), propagate=propagate,
                 **extra)
    return text(acc.run.runId)


def wait_run(c, run_id: str):
    # Subscribe first so completion between the snapshot and the wait is queued.
    c.ensure_subscription()
    status = c.call("RunStatusRequest", runId=run_id)
    if status.state in ENDED:
        return status
    note = c.wait_notification(
        lambda n: n.valueType == NotificationPayload.RunEvent
        and text(n.value.runId) == run_id
        and n.value.valueType == RunEventValue.RunStatusChanged
        and n.value.value.status.state in ENDED
    )
    return note.value.value.status


def wait_invocation(c, run_id, card_path, check):
    c.ensure_subscription()
    segments = card_path.split("/")
    try:
        rows = c.call("RunInvocationsRequest", runId=run_id, card=path(*segments)).records or []
    except RpcFailure as error:
        if error.kind != "not_found":
            raise
        rows = []
    if rows and check(rows[-1]):
        return rows[-1]

    # A finished run cannot produce another invocation update.
    def matches(note):
        if note.valueType != NotificationPayload.RunEvent or text(note.value.runId) != run_id:
            return False
        event = note.value
        if event.valueType == RunEventValue.RunFinished:
            return True
        if event.valueType != RunEventValue.RunCardChanged:
            return False
        row = event.value.invocation
        return [text(segment) for segment in row.card.segments] == segments and check(row)

    event = c.wait_notification(matches).value
    assert event.valueType == RunEventValue.RunCardChanged, (
        f"run {run_id} finished before {card_path} reached the expected invocation state"
    )
    return event.value.invocation


def run_and_wait(c, card_path, propagate: bool = True, concurrency: int | None = None):
    run_id = start_run(c, card_path, propagate, concurrency)
    return run_id, wait_run(c, run_id)
