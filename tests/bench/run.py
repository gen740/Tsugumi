#!/usr/bin/env python3
"""Opt-in local/remote execution baseline; no product code or pytest fixtures are patched."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import subprocess
import tempfile
import uuid

from measure_stages import (ROOT, UNAVAILABLE, Daemon, path, print_table, run_once,
                            summarize, summary, text)
from harness.real_cards import PYTHON_BINDINGS
from projects import SINK, card, conn, write_nix_project as write

SCENARIOS = ("value-fresh", "value-resident-50", "file-1gib", "file-sparse-1gib",
             "directory-10000", "file-input-1gib", "run-list-100")
# Requires the redesigned kit pin with sdk/python/default.nix.
PYTHON_RUN = 'sdk.pythonCard { src = ./src; outputs = ./_outputs.fbs; params = ./params.nix; inherit inputs; }'
# Every benchmark Card returns its own metrics as the declared `metrics` output.
METRICS = 'namespace bench; table Metrics { value:string; } '
VALUE = METRICS + 'table Number { value:ulong; } table Outputs { out:Number (required); metrics:Metrics (required); } root_type Outputs;'
PATH = ('attribute "tsugumi"; ' + METRICS +
        'table Outputs { out:string (tsugumi: "path"); metrics:Metrics (required); } root_type Outputs;')

# Metrics are emitted by the benchmark Card, not added to the product's SDK.
BODY = '''import hashlib
import json
import os
from pathlib import Path
import time

from card import Output
from bench.Metrics import MetricsT

OP = __OP__
SIZE = 1 << 30

def run(input):
    begin = time.perf_counter_ns()
    logical = allocated = read_bytes = 0
    out = None
    if OP in ("file-1gib", "file-sparse-1gib", "source"):
        # Path outputs are created under the working directory.
        target = Path("large.bin")
        with target.open("wb") as stream:
            if OP == "file-sparse-1gib":
                stream.truncate(SIZE)
            else:
                block = b"x" * (1 << 20)
                for _ in range(SIZE // len(block)):
                    stream.write(block)
        stat = target.stat()
        logical, allocated = stat.st_size, stat.st_blocks * 512
        out = str(target)
    elif OP == "directory-10000":
        target = Path("small")
        target.mkdir()
        for index in range(10000):
            member = target / f"{index:05d}.txt"
            member.write_bytes(b"x" * 128)
            stat = member.stat()
            logical += stat.st_size
            allocated += stat.st_blocks * 512
        out = str(target)
    elif OP == "file-input-1gib":
        digest = hashlib.sha256()
        with open(input.doc, "rb") as stream:
            while block := stream.read(1 << 20):
                digest.update(block)
                read_bytes += len(block)
        if read_bytes != SIZE:
            raise ValueError(f"expected {SIZE} input bytes, got {read_bytes}")
    if out is None:
        from bench.Number import NumberT
        out = NumberT(value=read_bytes or 1)
    elapsed = (time.perf_counter_ns() - begin) / 1e6
    metrics = json.dumps({"body_ms": elapsed, "pid": os.getpid(),
                          "output_logical_bytes": logical, "output_allocated_bytes": allocated,
                          "input_read_bytes": read_bytes})
    return Output(out=out, metrics=MetricsT(value=metrics))
'''


def workspace(out):
    out = out.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="tsg-bench-", dir=out.parent)).resolve()


def python_card(operation, *, service=None, resident=False, run_list=False):
    expression = f"builtins.genList (_: {PYTHON_RUN}) 100" if run_list else PYTHON_RUN
    # recordIO keeps the small metrics value for RunValueRead.
    result = {"type": "code", "sandbox": {},
              "reuseWorker": resident, "recordIO": True,
              "_outputs": PATH if operation in ("file-1gib", "file-sparse-1gib", "source", "directory-10000")
              else VALUE,
              "_nix": "{ params, inputs }: let " + PYTHON_BINDINGS + " in { run = " + expression + "; }\n"}
    if service:
        result["executionService"] = service
    if operation == "file-input-1gib":
        result["inputs"] = [{"name": "doc"}]
    return result


def make_project(root, scenario, service=None):
    definitions = {"work": python_card(scenario, service=service, resident=scenario == "value-resident-50",
                                        run_list=scenario == "run-list-100")}
    cards, connections = [card("work", "work")], []
    files = {"definitions/work/src/main.py": BODY.replace("__OP__", repr(scenario))}
    if scenario == "file-input-1gib":
        definitions["source"] = python_card("source", service=service)
        cards.insert(0, card("source", "source"))
        connections.append(conn("source", "out", "work", "doc"))
        files["definitions/source/src/main.py"] = BODY.replace("__OP__", repr("source"))
    write(root, definitions, cards, connections, files, name="Execution baseline")
    return root


@contextmanager
def broker_environment(endpoint=None, capability=None):
    changes = {"TSUGUMI_EXECUTION_BROKER": endpoint,
               "TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE": capability}
    previous = {key: os.environ.get(key) for key in changes}
    try:
        for key, value in changes.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = str(value)
        yield
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


class Remote:
    """Use an existing Host's authenticated RPC and broker, without starting a worker.

    Reuse the harness's Host wire client, but connect to a running Host instead
    of calling its process-owning constructor. Only this connection is owned here.
    """
    def __init__(self, ready_file, endpoint, public_key):
        from harness.host import Host
        self.call_wire = Host.call
        ready = json.loads(ready_file.read_text())
        self.request_id = 0
        self.service = "bench-" + uuid.uuid4().hex
        self.projects = []
        self.registered = False
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(60)
        self.stream = None
        try:
            self.socket.connect(ready["host_socket"])
            self.stream = self.socket.makefile("rwb", buffering=0)
            self.call("HelloRequest", token=ready["user_token"])
            self.call("ExecutionServicePutRequest", id=self.service, name="Execution baseline",
                      endpoint=endpoint, workerPublicKey=public_key, verify=True)
            self.registered = True
            services = self.call("ExecutionServicesRequest").services
            self.capabilities = next(s.capabilities for s in services if text(s.id) == self.service)
            self.endpoint = ready["execution_broker"]
        except BaseException:
            self.close()
            raise

    def call(self, name, **fields):
        return self.call_wire(self, name, **fields)

    def authorize(self, project_id, root, control):
        result = self.call("ExecutionProjectAuthorizeRequest", projectId=project_id,
                           path=str(root), controlDir=str(control), guest=False)
        if not result.available:
            raise RuntimeError("Host could not authorize this filesystem (project birth time is required)")
        self.projects.append(project_id)

    def close(self):
        try:
            for project_id in self.projects:
                self.call("ExecutionProjectRevokeRequest", projectId=project_id)
            if self.registered:
                self.call("ExecutionServiceRemoveRequest", id=self.service)
        finally:
            if self.stream:
                self.stream.close()
            self.socket.close()


def fixture_metrics(client, run):
    """Each invocation's declared `metrics` output, read back as a recorded value."""
    from harness.observations import decode_scalar, read_pages
    from Tsugumi.Data.ValueSide import ValueSide
    for row in run["invocations"]:
        payload = read_pages(client, "RunValueReadRequest", page_bytes=1 << 20, runId=run["run_id"],
                             card=path(*row["card"].split("/")), invocation=row["invocation"],
                             side=ValueSide.output, port="metrics")
        metrics = json.loads(decode_scalar(payload))
        row["fixture"] = metrics
        row["metrics"]["run_ms"] = metrics["body_ms"]
        for key in ("output_logical_bytes", "output_allocated_bytes", "input_read_bytes"):
            row["metrics"][key] = metrics[key]


def execute_bundle(root, scenario, timeout, remote, runs, metadata):
    """A fresh daemon per repetition; residents live for 50 serial requests."""
    project = make_project(root / "project", scenario, remote.service if remote else None)
    daemon = Daemon(project)
    metadata.update(project=str(project), daemon_directory=str(daemon.directory), setup=[],
                    daemon_processes_started=0, run_ids=[])
    capability = daemon.control / "execution-broker-capability"
    try:
        with broker_environment(remote.endpoint if remote else None, capability if remote else None):
            daemon.start()
        metadata["daemon_processes_started"] = 1
        client = daemon.connect()
        opened = client.open()
        if remote:
            metadata["project_id"] = text(opened.snapshot.projectId)
            remote.authorize(metadata["project_id"], project, daemon.control)
        if scenario == "file-input-1gib":
            setup = run_once(client, "source", timeout, cards=[("source",), ("work",)])
            fixture_metrics(client, setup)
            metadata["setup"].append(setup)
        count = 50 if scenario == "value-resident-50" else 1
        for _ in range(count):
            measured = run_once(client, "work", timeout, cards=[("work",)],
                                propagate=scenario != "file-input-1gib")
            fixture_metrics(client, measured)
            expected = 100 if scenario == "run-list-100" else 1
            if len(measured["invocations"]) != expected:
                raise RuntimeError(f"expected {expected} invocations, got {len(measured['invocations'])}")
            if remote and any(not r["execution_job_id"] or r["execution_service"] != remote.service
                              for r in measured["invocations"]):
                raise RuntimeError("expected every measured invocation to use the requested remote service")
            if scenario == "run-list-100" and any(r["worker_reused"] for r in measured["invocations"]):
                raise RuntimeError("fresh-worker run list unexpectedly reused a worker")
            if scenario == "run-list-100" and (
                    any(r["batch_size"] != 100 for r in measured["invocations"])
                    or [r["implementation_index"] for r in measured["invocations"]] != list(range(1, 101))):
                raise RuntimeError("expected one ordered 100-implementation execution batch")
            runs.append(measured)
            metadata["run_ids"].append(measured["run_id"])
    finally:
        daemon.stop()  # Retain the operator's run data, including on failure.


def stage_project(root, scenario):
    """Current-format versions of the original stage probe's three workloads."""
    if scenario == "collatz":
        shutil.copytree(ROOT / "demos/en/collatz", root)
        return root
    if scenario == "builtin-chain":
        definitions = {"trigger": {"type": "trigger"}, "sink": SINK,
                       "input": {"type": "input", "_params": "{ run = { value = 1; }; }",
                                 "_outputs": "table Value { value:ulong; } "
                                             "table Outputs { value:Value; } root_type Outputs;"}}
        cards = [card("start", "trigger"), card("src", "input")]
        connections = []
        previous = "src"
        for index in range(40):
            name = f"input{index}"
            cards.append(card(name, "input"))
            connections.append(conn(previous, "value", name, "start"))
            previous = name
        cards.append(card("sink", "sink"))
        connections.append(conn(previous, "value", "sink", "in"))
        write(root, definitions, cards, connections)
        return root
    raise ValueError(f"unknown stage scenario {scenario}")


def provenance(build_type):
    from harness.daemon import DAEMON, SDK, WORKER
    def revision(*args):
        return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()
    daemon_hash = hashlib.sha256()
    with DAEMON.open("rb") as stream:
        while block := stream.read(1 << 20):
            daemon_hash.update(block)
    return {"timestamp": datetime.now(timezone.utc).isoformat(), "machine": platform.node(),
            "platform": platform.platform(), "architecture": platform.machine(), "python": platform.python_version(),
            "revision": revision("rev-parse", "HEAD"), "dirty": bool(revision("status", "--porcelain")),
            "build_type": build_type, "daemon": str(DAEMON), "daemon_sha256": daemon_hash.hexdigest(),
            "worker": str(WORKER), "sdk": str(SDK), "max_concurrency": 1}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repetitions", "-n", type=int, default=5, help="N fresh-daemon bundles per scenario (default: 5)")
    parser.add_argument("--out", type=Path, default=Path("baseline.json"))
    parser.add_argument("--build-type", required=True, help="Record the actual build type, e.g. Release or Debug")
    parser.add_argument("--timeout", type=float, default=3600, help="Seconds per request/run")
    parser.add_argument("--remote-endpoint", help="Existing tls://host:port or unix:/path remote-worker listener")
    parser.add_argument("--remote-worker-key", help="Pinned ed25519:... worker public key")
    parser.add_argument("--remote-host-ready", type=Path, help="host.ready of an already-running, authorized Host")
    args = parser.parse_args()
    if args.repetitions < 1 or args.timeout <= 0:
        parser.error("repetitions and timeout must be positive")
    if any((args.remote_endpoint, args.remote_worker_key, args.remote_host_ready)) and not all(
            (args.remote_endpoint, args.remote_worker_key, args.remote_host_ready)):
        parser.error("remote mode requires --remote-endpoint, --remote-worker-key and --remote-host-ready")
    directory = workspace(args.out)
    report = {"schema_version": 1, "metadata": provenance(args.build_type), "workspace": str(directory),
              "repetitions": args.repetitions, "unavailable": UNAVAILABLE, "results": []}
    failed = False
    remote = None
    try:
        for mode in ("local", "remote") if args.remote_endpoint else ("local",):
            if mode == "remote":
                remote = Remote(args.remote_host_ready, args.remote_endpoint, args.remote_worker_key)
                report["remote"] = {"endpoint": args.remote_endpoint, "native_system": text(remote.capabilities.nativeSystem),
                                    "server_id": text(remote.capabilities.serverId),
                                    "worker_contract": remote.capabilities.workerContract,
                                    "build_type": "not exposed by the worker; record it alongside this report"}
            for scenario in SCENARIOS:
                result = {"path": mode, "scenario": scenario, "runs": [], "bundles": [], "summary": {}}
                report["results"].append(result)
                if mode == "remote" and scenario == "value-resident-50":
                    result.update(status="unavailable", error="Current contract rejects remote reuseWorker (tsugumid/document/src/graph.cc); no fresh-worker substitution was made.",
                                  smallest_change="Remote resident slots and a reuse request/result contract are required; outside this measurement-only step.")
                    continue
                try:
                    for index in range(args.repetitions):
                        bundle = directory / f"{mode}-{scenario}-{index + 1}"
                        bundle.mkdir()
                        runs, detail = [], {"repetition": index + 1, "workspace": str(bundle)}
                        result["bundles"].append(detail)
                        try:
                            execute_bundle(bundle, scenario, args.timeout, remote if mode == "remote" else None,
                                           runs, detail)
                            detail["request_wall_ms_per_invocation"] = sum(r["request_wall_ms"] for r in runs) / sum(
                                len(r["invocations"]) for r in runs)
                        finally:
                            result["runs"].extend(runs)
                    result["status"] = "ok"
                except Exception as error:
                    failed = True
                    result.update(status="failed", error=str(error))
                result["summary"] = summary(result["runs"])
                result["request_wall_ms_per_invocation"] = summarize(b.get("request_wall_ms_per_invocation") for b in result["bundles"])
                rows = [r for run in result["runs"] for r in run["invocations"]]
                result["observed_card_pids"] = sorted({r["fixture"]["pid"] for r in rows})
                result["distinct_local_card_pids"] = len(result["observed_card_pids"]) if mode == "local" else None
                result["worker_not_reused_invocations"] = sum(r["worker_reused"] is False for r in rows)
                result["worker_reused_invocations"] = sum(r["worker_reused"] is True for r in rows)
    except Exception as error:
        failed = True
        report["error"] = str(error)
    finally:
        if remote:
            try:
                remote.close()
            except Exception as error:
                failed = True
                report["remote_cleanup_error"] = str(error)
        args.out.write_text(json.dumps(report, indent=2) + "\n")
    print_table(report["results"])
    if report.get("error"):
        print(report["error"])
    print(f"JSON: {args.out.resolve()}  Retained workspace: {directory}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
