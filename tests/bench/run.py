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
import socket
import subprocess
import tempfile
import uuid

from measure_stages import (ROOT, UNAVAILABLE, Daemon, path, print_table, run_once,
                            summarize, summary, text)
from harness.demos import copy_demo
from harness.real_cards import PYTHON_BINDINGS
from projects import SINK, card, conn, write_nix_project as write
from test_worker_loading import native_card

SCENARIOS = ("value-fresh", "value-resident-50", "file-1gib", "file-sparse-1gib",
             "directory-10000", "file-input-1gib", "run-list-100")
# Requires the redesigned kit pin with sdk/python/default.nix.
PYTHON_RUN = 'sdk.pythonCard { src = ./src; inputs = ./_inputs.fbs; outputs = ./_outputs.fbs; inherit params; }'
# Every benchmark Card returns its own metrics as the declared `metrics` output.
METRICS = 'namespace bench; table Metrics { value:string; } '
VALUE = METRICS + 'table Number { value:ulong; } table Outputs { out:Number (required); metrics:Metrics (required); } root_type Outputs;'
ARTIFACT = METRICS + 'table Document {} table Outputs { out:Document; metrics:Metrics (required); } root_type Outputs;'
ARTIFACT_OPERATIONS = {"file-1gib", "file-sparse-1gib", "source", "directory-10000", "file-input-1gib"}

# Metrics are ordinary outputs in both fixtures.
BODY = '''import json
import os
import time

from card import Output
from bench.Metrics import MetricsT
from bench.Number import NumberT

def run(input):
    begin = time.perf_counter_ns()
    out = NumberT(value=1)
    elapsed = (time.perf_counter_ns() - begin) / 1e6
    return Output(out=out, metrics=MetricsT(value=json.dumps({"body_ms": elapsed,
        "pid": os.getpid(), "output_logical_bytes": 0, "output_allocated_bytes": 0,
        "input_read_bytes": 0})))
'''

ARTIFACT_BODY = r'''
    const auto started = std::chrono::steady_clock::now();
    constexpr std::string_view operation = __OP__;
    constexpr uint64_t size = 1ull << 30;
    uint64_t logical = 0, allocated = 0, read = 0;
    const auto account = [&](const fs::path& file) {
      struct stat info{};
      if (stat(file.c_str(), &info) != 0) return false;
      logical += info.st_size;
      allocated += info.st_blocks * 512;
      return true;
    };

    if (operation == "file-input-1gib") {
      const auto directory = open_artifact(call, in);
      if (directory.empty()) return 1;
      std::ifstream stream(fs::path(directory) / "large.bin", std::ios::binary);
      std::string block(1 << 20, '\0');
      while (stream.read(block.data(), block.size()) || stream.gcount() != 0)
        read += stream.gcount();
      if (read != size) return 2;
      out.number(read);
    } else {
      tsugumi_artifact_id id;
      const auto directory = fs::path(create_artifact(call, id));
      if (operation == "directory-10000") {
        for (unsigned index = 0; index < 10000; ++index) {
          const auto member = directory / (std::to_string(index) + ".txt");
          std::ofstream(member) << std::string(128, 'x');
          if (!account(member)) return 3;
        }
      } else {
        const auto file = directory / "large.bin";
        {
          std::ofstream stream(file, std::ios::binary);
          if (operation != "file-sparse-1gib") {
            const std::string block(1 << 20, 'x');
            for (uint64_t offset = 0; offset < size; offset += block.size())
              stream.write(block.data(), block.size());
          }
          if (!stream) return 4;
        }
        if (operation == "file-sparse-1gib") fs::resize_file(file, size);
        if (!account(file)) return 5;
      }
      out.artifact(id, "out");
    }

    const auto duration = std::chrono::steady_clock::now() - started;
    const auto elapsed = std::chrono::duration<double, std::milli>(duration).count();
    const auto metrics = "{\"body_ms\":" + std::to_string(elapsed) +
        ",\"pid\":" + std::to_string(getpid()) +
        ",\"output_logical_bytes\":" + std::to_string(logical) +
        ",\"output_allocated_bytes\":" + std::to_string(allocated) +
        ",\"input_read_bytes\":" + std::to_string(read) + "}";
    fb::FlatBufferBuilder payload;
    const auto value = payload.CreateString(metrics);
    const auto table = payload.StartTable();
    payload.AddOffset(4, value);
    out.add("metrics", payload, payload.EndTable(table));
'''


def workspace(out):
    out = out.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="tsg-bench-", dir=out.parent)).resolve()


def python_card(*, remote_system=None, resident=False, run_list=False):
    expression = f"builtins.genList (_: {PYTHON_RUN}) 100" if run_list else PYTHON_RUN
    # recordIO keeps the small metrics value for RunValueRead.
    result = {"type": "code", "sandbox": {},
              "reuseWorker": resident, "recordIO": "on",
              "_outputs": VALUE,
              "_nix": "{ params, inputs }: let " + PYTHON_BINDINGS + " in { run = " + expression + "; }\n"}
    if remote_system:
        result.update(system=remote_system, workerCapabilities=[63])
    return result


def artifact_card(operation, remote_system=None):
    reads_input = operation == "file-input-1gib"
    settings = {"recordIO": "on"}
    if remote_system:
        settings.update(system=remote_system, workerCapabilities=[63])
    return native_card(ARTIFACT_BODY.replace("__OP__", json.dumps(operation)),
                       inputs=["doc"] if reads_input else [],
                       outputs=VALUE if reads_input else ARTIFACT, **settings)


def make_project(root, scenario, remote_system=None):
    cards, connections = [card("work", "work")], []
    files = {}
    if scenario in ARTIFACT_OPERATIONS:
        definitions = {"work": artifact_card(scenario, remote_system)}
    else:
        definitions = {"work": python_card(remote_system=remote_system, resident=scenario == "value-resident-50",
                                            run_list=scenario == "run-list-100")}
        files["definitions/work/src/main.py"] = BODY
    if scenario == "file-input-1gib":
        definitions["source"] = artifact_card("source", remote_system)
        cards.insert(0, card("source", "source"))
        connections.append(conn("source", "out", "work", "doc"))
    write(root, definitions, cards, connections, files, name="Execution baseline")
    if scenario == "directory-10000":
        filename = root / "tsugumi.json"
        project = json.loads(filename.read_text())
        project["outputLimitEntries"] = 11000
        filename.write_text(json.dumps(project))
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
                      endpoint=endpoint, workerPublicKey=public_key, verify=True, workerCapabilities=[63])
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
    project = make_project(root / "project", scenario, text(remote.capabilities.nativeSystem) if remote else None)
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
        copy_demo("en", "collatz", root)
        return root
    if scenario == "builtin-chain":
        definitions = {"trigger": {"type": "trigger"}, "merge": {"type": "merge", "inputs": [{"name": "start"}]}, "sink": SINK}
        cards = [card("start", "trigger")]
        connections = []
        previous = "start"
        for index in range(41):
            name = f"merge{index}"
            cards.append(card(name, "merge"))
            connections.append(conn(previous, "out", name, "start"))
            previous = name
        cards.append(card("sink", "sink"))
        connections.append(conn(previous, "out", "sink", "in"))
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
