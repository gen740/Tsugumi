"""Manual Collatz R/Julia worker comparison, restored from 58079032^.

Run once per SDK/build, with the same daemon, nixpkgs and machine. The first
run and rerun remain separate; the current inputs/report/result contract and
external Card ports come from the repository's unmodified Collatz demo.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil

from measure_stages import Daemon, ROOT, UNAVAILABLE, path, print_table, run_once, summary, text
from run import workspace


def measure(language, directory, timeout):
    root = directory / f"collatz-{language}"
    shutil.copytree(ROOT / "demos/en/collatz", root)
    selected = f"collatz-{language}"
    document = json.loads((root / "tsugumi.json").read_text())
    document["canvas"] = {"cards": [{"id": name} for name in ("start", "initial", selected)], "connections": [
        {"from": {"card": "initial", "port": "value"}, "to": {"card": selected, "port": "n"}},
    ]}
    (root / "tsugumi.json").write_text(json.dumps(document))
    for definition in (root / "definitions").iterdir():
        if definition.name not in ("start", "initial", selected):
            shutil.rmtree(definition)
    for external in (root / "externals").iterdir():
        if external.name != selected:
            shutil.rmtree(external)
    daemon = Daemon(root, history_dir=directory / f"history-{language}")
    result = {"path": "local", "scenario": language, "runs": [], "summary": {},
              "project": str(root), "daemon_directory": str(daemon.directory)}
    try:
        daemon.start()
        client = daemon.connect()
        client.open()
        for repetition in (1, 2):
            measured = run_once(client, "start", timeout)
            measured["repetition"] = repetition
            # Keep the original workload correctness check: 27 takes 112 values.
            record = client.call("RunRecordRequest", runId=measured["run_id"],
                                 card=path(selected, "collect")).record
            if not text(record.display.value.text).startswith("112 values:"):
                raise RuntimeError("Collatz did not produce the expected 112-value sequence")
            measured["language_invocations"] = sum(r["card"].startswith(selected + "/") for r in measured["invocations"])
            measured["worker_reused_invocations"] = sum(r["worker_reused"] is True for r in measured["invocations"])
            result["runs"].append(measured)
    finally:
        daemon.stop()
        result["summary"] = summary(result["runs"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--languages", nargs="+", choices=("r", "jl"), default=["r", "jl"])
    parser.add_argument("--out", type=Path, default=Path("language-workers.json"))
    parser.add_argument("--timeout", type=float, default=1800)
    args = parser.parse_args()
    directory = workspace(args.out)
    report = {"workspace": str(directory), "unavailable": UNAVAILABLE, "results": []}
    try:
        for language in args.languages:
            report["results"].append(measure(language, directory, args.timeout))
    finally:
        args.out.write_text(json.dumps(report, indent=2) + "\n")
    print_table([{**result, "scenario": result["scenario"] + suffix, "runs": [run], "summary": summary([run])}
                 for result in report["results"]
                 for suffix, run in zip(("/first", "/rerun"), result["runs"])])


if __name__ == "__main__":
    main()
