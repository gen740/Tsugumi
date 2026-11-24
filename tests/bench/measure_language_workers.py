"""Manual Collatz R/Julia worker comparison, restored from 58079032^.

Run once per SDK/build, with the same daemon, nixpkgs and machine. The first
run and rerun remain separate; the Cards and their ports come from the
repository's Collatz demo, with only collect's recordIO turned on.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil

from measure_stages import Daemon, ROOT, UNAVAILABLE, path, print_table, run_once, summary
from run import workspace


def sequence_length(client, run_id, selected):
    """Length of the `values` vector of collect's last `sequence` output."""
    from flatbuffers.table import Table
    from harness.observations import read_pages
    from Tsugumi.Data.ValueSide import ValueSide
    card = path(selected, "collect")
    records = client.call("RunInvocationsRequest", runId=run_id, card=card).records
    payload = read_pages(client, "RunValueReadRequest", page_bytes=1 << 20, runId=run_id, card=card,
                         invocation=max(r.invocation for r in records), side=ValueSide.output, port="sequence")
    table = Table(payload, int.from_bytes(payload[:4], "little"))
    offset = table.Offset(4)
    return table.VectorLen(offset) if offset else 0


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
    # The sequence is read back as a recorded value.
    collect = root / "externals" / selected / "definitions" / "collect" / "_card.json"
    collect.write_text(json.dumps({**json.loads(collect.read_text()), "recordIO": True}))
    daemon = Daemon(root)
    result = {"path": "local", "scenario": language, "runs": [], "summary": {},
              "project": str(root), "daemon_directory": str(daemon.directory)}
    try:
        daemon.start()
        client = daemon.connect()
        client.open()
        for repetition in (1, 2):
            measured = run_once(client, "start", timeout)
            measured["repetition"] = repetition
            # Keep the original workload correctness check: 27 takes 112 values,
            # read from collect's declared `sequence` output.
            if sequence_length(client, measured["run_id"], selected) != 112:
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
