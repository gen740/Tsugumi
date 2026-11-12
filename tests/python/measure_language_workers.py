"""Measure the unchanged Collatz R/Julia branches with a selected SDK/worker.

Run this script once with TSUGUMI_SDK_DIR and TSUGUMI_WORKER pointing at the
baseline, then with the new SDK/worker. The daemon and nixpkgs stay identical.
The demo starts at 27 (112 values). Each language runs alone, twice, with
sandboxing disabled on a development host. First-run build time is reported
separately from the sum of language Card execute stages.
"""
from __future__ import annotations

import json
import shutil
import tempfile
from pathlib import Path

from conftest import ROOT, Daemon
from projects import run_and_wait
from tsugumi_client import path, text
from Tsugumi.Data.Phase import Phase
from Tsugumi.Data.RunState import RunState


def measure(language):
    with tempfile.TemporaryDirectory(prefix="collatz-workers-") as temporary:
        root = Path(temporary) / "collatz"
        shutil.copytree(ROOT / "demos/en/collatz", root)
        selected = f"collatz-{language}"
        document = json.loads((root / "tsugumi.json").read_text())
        document["canvas"] = {"cards": [{"id": c} for c in ("start", "initial", selected)], "connections": [
            {"from": {"card": "start", "port": "out"}, "to": {"card": "initial", "port": "start"}},
            {"from": {"card": "initial", "port": "value"}, "to": {"card": selected, "port": "n"}},
        ]}
        (root / "tsugumi.json").write_text(json.dumps(document))
        for directory in (root / "definitions").iterdir():
            if directory.name not in ("start", "initial", selected):
                shutil.rmtree(directory)
        for directory in (root / "externals").iterdir():
            if directory.name != selected:
                shutil.rmtree(directory)
        for card in root.rglob("_card.json"):
            value = json.loads(card.read_text())
            if value.get("language"):
                value["sandbox"] = False
                card.write_text(json.dumps(value))
        environment = Daemon(root).start()
        try:
            client = environment.client()
            client.hello()
            for repetition in (1, 2):
                run_id, status = run_and_wait(client, "start", timeout=1800)
                assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
                totals = {}
                calls = reused = 0
                for card in ("check", "halve", "triple", "collect"):
                    records = client.call("RunInvocationsRequest", runId=run_id, card=path(selected, card), limit=500).records
                    for record in records:
                        calls += 1
                        reused += bool(record.workerReused)
                        for stage in record.stages or []:
                            name = next(k for k, v in vars(Phase).items() if v == stage.phase)
                            totals[name] = totals.get(name, 0) + stage.end - stage.start
                display = client.call("RunRecordRequest", runId=run_id, card=path(selected, "collect")).record.display
                assert text(display.value.text).startswith("112 values:"), text(display.value.text)
                print(json.dumps({"language": language, "run": repetition, "wall_ms": status.endedAt - status.startedAt,
                                  "calls": calls, "reused": reused, "stages_ms": totals}), flush=True)
        finally:
            environment.cleanup()


if __name__ == "__main__":
    for language in ("r", "jl"):
        measure(language)
