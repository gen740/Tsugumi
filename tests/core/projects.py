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
from harness.document import params_nix
from harness.nix_path import declarations, test_lock_entries, write_lock

MSG_FBS = """
namespace demo;
table Msg { text:string; }
table Cond { value:bool; }
table Outputs { msg:Msg (required); cond:Cond; }
root_type Outputs;
"""

# Independent producer fixture: build the two FlatBuffers with flatc using
# evaluated Nix params, then expose exactly the Input Card output layout.
MSG_INPUT_NIX = """{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  msg = pkgs.writeText "msg.json" (builtins.toJSON { inherit (params) text; });
  cond = pkgs.writeText "cond.json" (builtins.toJSON { inherit (params) value; });
in {
  run = pkgs.runCommand "message-input" { nativeBuildInputs = [ pkgs.flatbuffers ]; } ''
    mkdir -p "$out/share/tsugumi/input/ports"
    flatc --binary --strict-json --root-type demo.Msg ${./_outputs.fbs} ${msg}
    flatc --binary --strict-json --root-type demo.Cond ${./_outputs.fbs} ${cond}
    mv *-msg.bin "$out/share/tsugumi/input/ports/msg"
    mv *-cond.bin "$out/share/tsugumi/input/ports/cond"
  '';
}
"""

SINK = {"type": "collect"}

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


def write(root: Path, defs: dict, cards: list, conns: list, files: dict | None = None, name: str = "test",
          *, locked=()) -> None:
    """Writes a project. `defs` are the bodies of _card.json by name; a body
    may carry "_outputs" (its _outputs.fbs), "_params" (its params.nix) and
    "_nix" (its default.nix).
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
                  {k: v for k, v in files.items() if k.startswith("definitions/")}, name=cid, locked=locked)
            doc = json.loads((root / "externals" / cid / "tsugumi.json").read_text())
            doc["boundary"] = {side: [dict(name=name, **ep) for name, ep in ports.items()]
                               for side, ports in inner["boundary"].items()}
            put(root / "externals" / cid / "tsugumi.json", json.dumps(doc))
            put(folder / "_card.json", json.dumps({"type": "subgraph", "external": cid}))
            continue
        d = c.get("definition")
        body = copy.deepcopy(defs[d]) if d else {"type": "draft"}
        outputs, nix = body.pop("_outputs", None), body.pop("_nix", None)
        params = body.pop("_params", None)
        for k, v in c.items():
            if k not in ("id", "definition", "params"):
                body[k] = v
        values = {**body.pop("params", {}), **c.get("params", {})}
        if values:
            params = params_nix(values)
        # Card-local file parameters need no rewriting when a definition is reused.
        mine, theirs = f"definitions/{cid}/", f"definitions/{d}/"
        put(folder / "_card.json", json.dumps(body))
        if outputs:
            put(folder / "_outputs.fbs", outputs)
        if nix:
            put(folder / "default.nix", nix)
        if params is not None:
            put(folder / "params.nix", params)
        for rel, data in files.items():
            if d and rel.startswith(theirs):
                put(root / (mine + rel[len(theirs):]), data)
    defs_names = {f"definitions/{d}/" for d in defs}
    for rel, data in files.items():
        if not any(rel.startswith(p) for p in defs_names):
            put(root / rel, data)
    put(root / "tsugumi.json", json.dumps({
        "name": name, "nixPath": declarations(locked),
        "canvas": {"cards": [{"id": c["id"]} for c in cards], "connections": conns}}))
    if locked:
        write_lock(root, locked)


def write_nix_project(root, defs, cards, conns, files=None, name="test", *, locked=None):
    """Write a project for the real evaluator with pinned fixture dependencies."""
    write(root, defs, cards, conns, files, name, locked=test_lock_entries() if locked is None else locked)


def builtin_project(root: Path, text: str = "hello", extra_cards=(), extra_conns=()) -> None:
    """An input Card wired to a sink, with a trigger for whole-workflow runs."""
    defs = {
        "trigger": {"type": "trigger"},
        "source": {"type": "input", "params": {"text": "x", "value": True},
                   "_outputs": MSG_FBS, "_nix": MSG_INPUT_NIX},
        "sink": SINK,
        "merge": {"type": "merge", "inputs": [{"name": "a"}, {"name": "b"}]},
    }
    # The input Card is a start Card (no inputs).
    write(root, defs, [card("start", "trigger"), card("src", "source", params={"text": text}), card("out", "sink"),
                       *extra_cards],
          [conn("src", "msg", "out", "in"), *extra_conns], name="builtin",
          locked=[entry for entry in test_lock_entries() if entry["name"] == "nixpkgs"])


def start_run(c, card_path, propagate: bool = True, concurrency: int | None = 4) -> str:
    """Starts a run from a Card ("id", "outer/inner" or a list of ids); its run_id."""
    c.ensure_subscription()
    segments = card_path.split("/") if isinstance(card_path, str) else list(card_path)
    rev = c.call("ProjectInspectRequest").rev
    extra = {"maxConcurrency": concurrency} if concurrency else {}
    c.ensure_subscription()
    acc = c.call("RunRequest", baseRev=rev, opId=new_id(), origin="test", card=path(*segments), propagate=propagate,
                 **extra)
    return text(acc.run.runId)


def wait_run(c, run_id: str, *, timeout=None):
    """Follow terminal status. Benchmarks may bound their measurement window."""
    # Subscribe first so completion between the snapshot and the wait is queued.
    c.ensure_subscription()
    status = c.call("RunStatusRequest", runId=run_id)
    if status.state in ENDED:
        return status
    return wait_run_notification(c, run_id, timeout=timeout)


def wait_run_notification(c, run_id: str, *, timeout=None):
    """Wait for a terminal status event on an existing subscription."""
    note = c.wait_notification(
        lambda n: n.valueType == NotificationPayload.RunEvent
        and text(n.value.runId) == run_id
        and n.value.valueType == RunEventValue.RunStatusChanged
        and n.value.value.status.state in ENDED,
        timeout=timeout,
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
    for row in reversed(rows):
        if check(row):
            return row

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


def wait_for_display(c, run_id, check=lambda _: True):
    """Wait for a display in an open session, failing if the run ends first."""
    def matches(note):
        if note.valueType == NotificationPayload.DisplayUpdated:
            display = note.value.display
            return check(display)
        return (note.valueType == NotificationPayload.RunEvent
                and text(note.value.runId) == run_id
                and note.value.valueType == RunEventValue.RunFinished)

    note = c.wait_notification(matches)
    assert note.valueType == NotificationPayload.DisplayUpdated, (
        f"run {run_id} ended before its expected display"
    )
    return note


def wait_for_log(c, run_id, card_path, stream, check, *, invocation=1):
    """Read live log updates until the fixture's marker or the end of the run."""
    c.ensure_subscription()
    segments = card_path.split("/")

    def matches(note):
        event = note.value
        if note.valueType not in (NotificationPayload.LogAvailable, NotificationPayload.RunEvent):
            return False
        if text(event.runId) != run_id:
            return False
        if note.valueType == NotificationPayload.RunEvent:
            return event.valueType == RunEventValue.RunFinished
        return (event.invocation == invocation and event.card is not None
                and [text(segment) for segment in event.card.segments] == segments)

    while True:
        log = c.call("LogRangeRequest", runId=run_id, card=path(*segments),
                     invocation=invocation, stream=stream)
        if check(log):
            return log
        status = c.call("RunStatusRequest", runId=run_id)
        if status.state in ENDED:
            # Read once more after completion; the final log can be persisted
            # between the previous read and the terminal snapshot.
            log = c.call("LogRangeRequest", runId=run_id, card=path(*segments),
                         invocation=invocation, stream=stream)
            assert check(log), f"run {run_id} finished before {card_path} logged its marker"
            return log
        c.wait_notification(matches)


def run_and_wait(c, card_path, propagate: bool = True, concurrency: int | None = None):
    run_id = start_run(c, card_path, propagate, concurrency)
    return run_id, wait_run(c, run_id)
