"""Inputs and controllable dependencies; no inspection of core's private state."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

from projects import wait_run as wait_run
from tsugumi_client import RpcFailure, change, new_id, path, text

from .daemon import BUILD, ROOT
from .file_events import wait_for_file

STRING_OUTPUT = 'table Value { value:string; } table Outputs { out:Value; } root_type Outputs;'
FILE_OUTPUT = ('attribute "tsugumi"; '
               'table Outputs { doc:string (tsugumi: "path"); } root_type Outputs;')


class Gate:
    def __init__(self, directory: Path, name: str):
        self.entered = directory / (name + ".started")
        self.released = directory / (name + ".release")

    def wait(self):
        wait_for_file(self.entered)

    def release(self):
        self.released.touch()

    def action(self):
        return {"op": "gate", "entered": str(self.entered), "release": str(self.released)}


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

    def card(self, name, definition, *, outputs=None, plan=None, sources=None):
        files = dict(sources or {})
        if outputs is not None:
            files["_outputs.fbs"] = outputs
        if "sdk.pythonCard" in files.get("default.nix", ""):
            files.setdefault("params.nix", "{ }\n")
        if plan is not None:
            # This source is interpreted only by the test build adapter.
            files["default.nix"] = "{ params, inputs }: { }"
            files["mock-build.json"] = json.dumps(plan)
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
    client.ensure_subscription()
    accepted = client.call("RunRequest", baseRev=client.call("ProjectInspectRequest").rev,
                           opId=new_id(), origin="ui:test", card=path(*card.split("/")), **fields)
    return text(accepted.run.runId)
