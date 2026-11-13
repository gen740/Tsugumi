"""Inputs and controllable dependencies; no inspection of core's private state."""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState

from .daemon import BUILD, ROOT

ENDED = {RunState.succeeded, RunState.failed, RunState.cancelled}
STRING_OUTPUT = 'table Value { value:string; } table Outputs { out:Value; } root_type Outputs;'
FILE_OUTPUT = ('attribute "tsugumi"; table File { value:string; } '
               'table Outputs { doc:File (tsugumi:"file"); } root_type Outputs;')


class Gate:
    def __init__(self, directory: Path, name: str):
        self.entered = directory / (name + ".started")
        self.released = directory / (name + ".release")

    def wait(self, timeout=5):
        end = time.monotonic() + timeout
        while not self.entered.exists():
            if time.monotonic() >= end:
                raise TimeoutError(f"dependency did not reach gate {self.entered}")
            time.sleep(0.01)

    def release(self):
        self.released.touch()

    def action(self):
        return {"op": "gate", "entered": str(self.entered), "release": str(self.released), "timeout": 10}


def worker_launcher(directory: Path) -> Path:
    launcher = directory / "worker"
    bindings = Path(os.environ.get("TSUGUMI_PY_BINDINGS", BUILD / "generated/python")).resolve()
    launcher.write_text(f"#!{sys.executable}\nimport sys, runpy\nsys.path.insert(0, {str(bindings)!r})\n"
                        f"runpy.run_path({str(ROOT / 'tests/core/doubles/worker.py')!r}, run_name='__main__')\n")
    launcher.chmod(0o700)
    return launcher


class Project:
    def __init__(self, directory: Path):
        self.directory = directory
        self.cards = {}
        self.connections = []

    def card(self, name, definition, *, outputs=None, manifest=None, plan=None, sources=None):
        files = dict(sources or {})
        if outputs is not None:
            files["_outputs.fbs"] = outputs
        if manifest is not None or plan is not None:
            # This source is interpreted only by the test build adapter.
            files["default.nix"] = "{ pkgs, inputs, params }: { }"
            files["mock-build.json"] = json.dumps({**(plan or {}), **({"manifest": manifest} if manifest else {})})
        self.cards[name] = (definition, files)
        return self

    def connect(self, source, output, target, input):
        self.connections.append({"from": {"card": source, "port": output},
                                 "to": {"card": target, "port": input}})
        return self

    def save(self, client):
        snap = client.call("ProjectInspectRequest")
        doc = json.loads(text(snap.projectJson))
        doc["canvas"] = {"cards": [{"id": name} for name in self.cards], "connections": self.connections}
        changes = [change("SetProject", json=json.dumps(doc))]
        for name, (definition, files) in self.cards.items():
            changes.append(change("PutDefinition", id=name, cardJson=json.dumps(definition)))
            for filename, content in files.items():
                relative = f"definitions/{name}/{filename}"
                fields = {}
                try:
                    info = client.call("ProjectFileStatRequest", path=relative, atRev=snap.rev)
                    fields["baseHash"] = text(info.hash)
                except RpcFailure as failure:
                    if failure.kind != "not_found":
                        raise
                changes.append(change("PutSource", path=relative, bytes=content.encode(), **fields))
        return client.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="ui:test", changes=changes)


def run(client, card="work", **fields):
    accepted = client.call("RunRequest", baseRev=client.call("ProjectInspectRequest").rev,
                           opId=new_id(), origin="ui:test", card=path(*card.split("/")), **fields)
    return text(accepted.run.runId)


def wait_run(client, run_id, timeout=60):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        status = client.call("RunStatusRequest", runId=run_id, timeout=min(2, max(0.01, end-time.monotonic())))
        if status.state in ENDED:
            return status
        time.sleep(0.01)
    raise TimeoutError(f"run {run_id} did not finish in {timeout}s")
