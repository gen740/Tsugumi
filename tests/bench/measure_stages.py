"""Manual stage/history probe, restored from 58079032^ and adapted to today's RPC.

Also supplies the collector used by run.py. This file is not a pytest test.
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/core"))

from harness.daemon import Daemon  # noqa: E402
from harness.scenario import wait_run  # noqa: E402
from tsugumi_client import RpcFailure, new_id, path, text  # noqa: E402
from Tsugumi.Data.Phase import Phase  # noqa: E402
from Tsugumi.Data.RunState import RunState  # noqa: E402
from Tsugumi.Data.StageOutcome import StageOutcome  # noqa: E402
from Tsugumi.RPC.NotificationPayload import NotificationPayload  # noqa: E402
from Tsugumi.RPC.RunEventValue import RunEventValue  # noqa: E402

PHASES = {v: k for k, v in vars(Phase).items() if isinstance(v, int)}
OUTCOMES = {v: k for k, v in vars(StageOutcome).items() if isinstance(v, int)}

UNAVAILABLE = {
    "start_ms": {
        "reason": "No persisted worker spawn/ready timestamps; execute includes startup and input staging.",
        "smallest_change": "Record monotonic spawn/acquire begin and worker-ready durations per invocation.",
    },
    "worker_invocation_ms": {
        "reason": "No persisted worker-side invocation duration including SDK encoding. The benchmark body reports its own run_ms; local RunTimeline measures the whole call, and remote execute includes build, polling and import.",
        "smallest_change": "Persist worker-side invocation duration (not cross-machine absolute timestamps) in the result and history.",
    },
    "output_handover_ms": {
        "reason": "Copies/import, validation and worker cleanup have no separate persisted interval.",
        "smallest_change": "Time the existing output-copy/import boundary and persist its duration in InvocationSummary.",
    },
    "remote_queue_ms": {
        "reason": "Core readyAt measures the core lane queue; the remote worker's acceptance-to-start interval is not returned in history.",
        "smallest_change": "Return a worker-clock queue duration with the remote result and persist it in history.",
    },
    "remote_build_ms": {
        "reason": "Remote realization happens inside the broader core execution envelope; the result exposes built/cache status but no realization duration.",
        "smallest_change": "Return the duration around remote Nix realization with the result and persist it in history.",
    },
    "bytes_copied": {
        "reason": "Logical payload size and allocated file blocks do not count copies, reflinks, sparse expansion, archives or transport retries.",
        "smallest_change": "Accumulate successful bytes at existing staging-copy and remote blob-transfer loops, by direction, and return the counters.",
    },
    "processes_spawned": {
        "reason": "History records resident reuse, not all spawns (including standby refill, Nix, shells, helpers, failed attempts and remote supervisors). A non-reused call can consume a prefetched worker. PID sampling would miss short-lived children.",
        "smallest_change": "Count successful spawn calls in the core and worker supervisor and return per-invocation counters, with build processes separate.",
    },
}


def summarize(values):
    """Median and nearest-rank p95; retain n so a one-sample p95 is recognizable."""
    values = sorted(v for v in values if v is not None)
    if not values:
        return None
    return {"n": len(values), "median": statistics.median(values),
            "p95": values[math.ceil(0.95 * len(values)) - 1], "total": sum(values)}


def invocation_rows(client, run_id, cards=None):
    if cards is None:
        cards = [tuple(text(s) for s in r.card.segments)
                 for r in client.call("ProjectInspectRequest").resolution]
    rows = []
    for segments in cards:
        cursor = None
        while True:
            fields = {"cursor": cursor} if cursor else {}
            try:
                page = client.call("RunInvocationsRequest", runId=run_id, card=path(*segments), limit=500, **fields)
            except RpcFailure as error:
                if error.kind == "not_found":
                    break  # A conditional branch in the inspected graph never ran.
                raise
            rows.extend(page.records or [])
            cursor = text(page.nextCursor)
            if not cursor:
                break
    return rows


def collect(client, run_id, cards=None):
    """Read durable summaries and already-received worker-call timeline events."""
    calls = {}
    with client._cv:
        notes = list(client.notifications)
        client.notifications.clear()
    for note in notes:
        if note.valueType != NotificationPayload.RunEvent:
            continue
        event = note.value
        if text(event.runId) != run_id or event.valueType != RunEventValue.RunTimeline:
            continue
        tick = event.value
        if tick.phase == Phase.execute and tick.start is not None and tick.end is not None:
            key = (tuple(text(s) for s in tick.card.segments), tick.invocation)
            calls[key] = tick.end - tick.start
    rows = []
    for record in invocation_rows(client, run_id, cards):
        segments = tuple(text(s) for s in record.card.segments)
        stages = [{"phase": PHASES[s.phase], "outcome": OUTCOMES[s.outcome],
                   "start": s.start, "end": s.end,
                   "ms": s.end - s.start if s.end is not None and s.start is not None else None}
                  for s in record.stages or []]
        phases = {name: sum(s["ms"] for s in stages if s["phase"] == name and s["ms"] is not None)
                  for name in PHASES.values() if any(s["phase"] == name and s["ms"] is not None for s in stages)}
        ends = [s["end"] for s in stages if s["end"] is not None]
        if record.endedAt is not None:
            ends.append(record.endedAt)
        metrics = {"wall_ms": max(ends) - record.startedAt if ends and record.startedAt is not None else None,
                   "queue_ms": record.startedAt - record.readyAt
                   if record.startedAt is not None and record.readyAt is not None else None,
                   **{name + "_ms": phases.get(name) for name in ("prepare", "evaluate", "build", "execute", "validate", "save")},
                   "worker_call_ms": calls.get((segments, record.invocation)),
                   "run_ms": None,  # Filled from the benchmark's Report, when present.
                   **{name: None for name in UNAVAILABLE}}
        rows.append({"card": "/".join(segments), "invocation": record.invocation,
                     "state": record.state, "started_at": record.startedAt, "ended_at": record.endedAt,
                     "duration_ms": record.durationMs, "worker_reused": record.workerReused,
                     "build_cached": record.buildCached, "execution_service": text(record.executionService),
                     "execution_job_id": text(record.executionJobId), "environment_id": text(record.environmentId),
                     "execution_system": text(record.executionSystem), "emulator": text(record.emulator),
                     "batch_activation": record.batchActivation, "batch_index": record.batchIndex,
                     "batch_size": record.batchSize, "implementation_index": record.implementationIndex,
                     "stages": stages, "metrics": metrics})
    return rows


def run_once(client, start, timeout, *, cards=None, propagate=True):
    revision = client.call("ProjectInspectRequest").rev
    client.ensure_subscription()
    begin = time.perf_counter_ns()
    accepted = client.call("RunRequest", baseRev=revision, opId=new_id(), origin="bench",
                           card=path(*start.split("/")), propagate=propagate, maxConcurrency=1)
    run_id = text(accepted.run.runId)
    status = wait_run(client, run_id, timeout=timeout)
    wall_ms = (time.perf_counter_ns() - begin) / 1e6
    if status.state != RunState.succeeded:
        failure = text(status.failure.diagnostic) if status.failure else str(status.state)
        raise RuntimeError(f"run {run_id}: {failure}")
    return {"run_id": run_id, "request_wall_ms": wall_ms, "state": status.state,
            "invocations": collect(client, run_id, cards)}


def summary(runs):
    rows = [row for run in runs for row in run["invocations"]]
    keys = rows[0]["metrics"] if rows else ["wall_ms", "queue_ms", *UNAVAILABLE]
    return {key: summarize(row["metrics"].get(key) for row in rows) for key in keys}


def print_table(results):
    columns = [("request_wall_ms_per_invocation", "RPC/inv"), ("wall_ms", "wall"), ("queue_ms", "queue"), ("prepare_ms", "prepare"),
               ("evaluate_ms", "eval"), ("build_ms", "build"), ("execute_ms", "execute"),
               ("worker_call_ms", "call"), ("validate_ms", "validate"), ("save_ms", "save"),
               ("start_ms", "start"), ("run_ms", "run"), ("output_handover_ms", "handover"),
               ("bytes_copied", "copy B"), ("processes_spawned", "spawns")]
    print("All time cells: median/p95 ms; '-' = unavailable. Counts cover measured invocations only.")
    print(f"{'path/scenario':<31} {'n':>6} " + " ".join(f"{label:>13}" for _, label in columns)
          + f" {'Card PIDs':>13}")
    for result in results:
        cells = []
        for key, _ in columns:
            value = result.get(key) if key == "request_wall_ms_per_invocation" else result["summary"].get(key)
            cells.append(f"{value['median']:.2f}/{value['p95']:.2f}" if value else "-")
        count = sum(len(run["invocations"]) for run in result["runs"])
        print(f"{result['path'] + '/' + result['scenario']:<31} {count:>6} "
              + " ".join(f"{cell:>13}" for cell in cells)
              + f" {str(result.get('distinct_local_card_pids', '-')):>13}")
        if result.get("error"):
            print("  " + result["error"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", choices=["collatz", "builtin-chain", "large-values"])
    parser.add_argument("--project", type=Path, help="Probe this current-format project instead of generating a fixture")
    parser.add_argument("--start", default="start")
    parser.add_argument("--out", type=Path, default=Path("stages.json"))
    parser.add_argument("--timeout", type=float, default=3600)
    args = parser.parse_args()
    from run import stage_project, workspace
    directory = workspace(args.out)
    root = args.project.resolve() if args.project else stage_project(directory / "project", args.scenario)
    daemon = Daemon(root)
    result = {"path": "local", "scenario": args.scenario, "runs": [], "summary": {},
              "workspace": str(directory), "daemon_directory": str(daemon.directory)}
    try:
        daemon.start()
        client = daemon.connect()
        client.open()
        for _ in range(2):
            result["runs"].append(run_once(client, args.start, args.timeout))
    finally:
        daemon.stop()
        result["summary"] = summary(result["runs"])
        args.out.write_text(json.dumps({"results": [result], "unavailable": UNAVAILABLE}, indent=2) + "\n")
    print_table([{**result, "scenario": args.scenario + suffix, "runs": [run], "summary": summary([run])}
                 for suffix, run in zip(("/first", "/rerun"), result["runs"])])


if __name__ == "__main__":
    main()
