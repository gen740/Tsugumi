"""Run an exported Project once, inside the pack guest, and write its results.

Called by the guest's /init in run mode. The Project is a copy of what the
author exported and runs with the values fixed at export time. The run is an
ordinary run of the Workflow from its trigger. Nothing here schedules Cards: tsugumid does, through the
same RPC the apps use (tsugumi-rpc, one JSON request per line).

    python3 run.py PROJECT REQUEST RESULT

PROJECT  writable copy of the exported Project
REQUEST  request.json from the Mac: {"start": "<trigger>"}
RESULT   folder for summary.json and cards/<card>/<invocation>/...

The copy records every Code Card's inputs and outputs (recordIO), whatever the
author chose: the recipient gets the values back. recordIO is
a setting, so this changes no build.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import time
import uuid
from pathlib import Path

ENDED = {"succeeded", "failed", "cancelled"}


def stage(text):
    print(f"PACK-STAGE {text}", flush=True)


class RpcFailure(Exception):
    pass


class Rpc:
    """Lock-step requests through tsugumi-rpc; events are skipped."""

    def __init__(self, endpoint, schema):
        self.process = subprocess.Popen(
            [os.environ["TSUGUMI_RPC"], "--endpoint", endpoint, "--schema", schema, "--timeout-ms", "3600000"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.next_id = 1

    def call(self, kind, value):
        request_id = self.next_id
        self.next_id += 1
        message = {"payload_type": "Request", "request_id": request_id,
                   "payload": {"value_type": kind, "value": value}}
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RpcFailure(f"{kind}: the RPC connection closed")
            reply = json.loads(line)
            if "transport_error" in reply:
                raise RpcFailure(f"{kind}: {reply['transport_error']}")
            if reply.get("request_id") != request_id or reply.get("payload_type") != "Reply":
                continue
            payload = reply.get("payload", {})
            if payload.get("value_type") == "Failure":
                failure = payload.get("value", {})
                raise RpcFailure(f"{kind}: {failure.get('kind')}: {failure.get('diagnostic', '')}")
            return payload.get("value", {})

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=10)


def record_all(project):
    """Set recordIO on for every Code Card of the copy."""
    for path in (project / "definitions").glob("*/_card.json"):
        definition = json.loads(path.read_text())
        if definition["type"] == "subgraph":
            record_all(project / "externals" / path.parent.name)
        elif definition["type"] == "code" and definition["recordIO"] != "on":
            definition["recordIO"] = "on"
            path.write_text(json.dumps(definition, indent=2, ensure_ascii=False) + "\n")


def wired_outputs(project):
    """(card, port) pairs that feed another Card: the rest are the Workflow's results."""
    connections = json.loads((project / "tsugumi.json").read_text()).get("canvas", {}).get("connections", [])
    # A Draft end has no port.
    return {(c["from"]["card"], c["from"].get("port")) for c in connections}


def triggers(project):
    cards = json.loads((project / "tsugumi.json").read_text()).get("canvas", {}).get("cards", [])
    found = []
    for card in cards:
        path = project / "definitions" / card["id"] / "_card.json"
        if path.is_file() and json.loads(path.read_text()).get("type") == "trigger":
            found.append(card["id"])
    return found


def start_core(project, work):
    control = work / "control"
    control.mkdir(parents=True, exist_ok=True)
    socket = work / "core.sock"
    log = open(work / "tsugumid.log", "w")
    process = subprocess.Popen(
        [os.environ["TSUGUMID"], "--project", str(project), "--unix", str(socket),
         "--control-dir", str(control), "--cpus", str(os.cpu_count() or 2),
         "--scratch-dir", str(work / "scratch"), "--run-scratch-dir", str(work / "run-scratch")]
        # The builds made at export: no Card is evaluated in the app.
        + (["--prepared-cards", os.environ["PACK_PREPARED"]] if os.environ.get("PACK_PREPARED") else []),
        stdout=log, stderr=subprocess.STDOUT)
    ready = control / "server.ready"
    for _ in range(600):
        if ready.exists():
            return process, socket
        if process.poll() is not None:
            break
        time.sleep(0.05)
    log.flush()
    raise SystemExit("tsugumid did not start:\n" + (work / "tsugumid.log").read_text()[-4000:])


def decode_value(value, folder):
    """A port value as JSON (flatc with the value's own schema), raw bytes beside it."""
    name = value["name"]
    data = bytes(value.get("inline_bytes", []))
    if not data:
        artifacts = value.get("artifacts", [])
        return {"artifacts": artifacts} if artifacts else None
    raw = folder / f"{name}.bin"
    raw.write_bytes(data)
    entry = {"bytes": f"{name}.bin"}
    entry["artifacts"] = value.get("artifacts", [])
    schema = folder / f".{name}.bfbs"
    schema.write_bytes(bytes(value.get("schema_bfbs", [])))
    result = subprocess.run(
        [os.environ["FLATC"], "--json", "--strict-json", "--raw-binary", "--defaults-json",
         "--root-type", value["root_type"], "-o", str(folder), str(schema), "--", str(raw)],
        capture_output=True, text=True)
    schema.unlink()
    produced = folder / f"{name}.json"
    if result.returncode != 0 or not produced.exists():
        entry["decode_error"] = result.stderr.strip()
    else:
        entry["json"] = f"{name}.json"
    return entry


def read_log(rpc, run_id, card, invocation, stream):
    text, offset = "", 0
    while True:
        part = rpc.call("LogRangeRequest", {"run_id": run_id, "card": card, "invocation": invocation,
                                            "stream": stream, "offset": offset, "limit_bytes": 262144})
        text += part.get("text", "")
        following = part.get("next_offset")
        if not part.get("text") or following is None or following <= offset or following >= part.get("size_bytes", 0):
            return text
        offset = following


def export_artifacts(rpc, run_id, result):
    cursor = None
    while True:
        request = {"run_id": run_id}
        if cursor:
            request["cursor"] = cursor
        page = rpc.call("ArtifactListRequest", request)
        for artifact in page.get("artifacts", []):
            artifact_id = artifact["artifact_id"]
            if re.fullmatch(r"[1-9][0-9]*\.[0-9]+", artifact_id) is None:
                raise ValueError("malformed artifact id")
            root = result / "artifacts" / artifact_id
            root.mkdir(parents=True, exist_ok=True)
            for member in artifact.get("members", []):
                relative = Path(member["path"])
                if relative.is_absolute() or ".." in relative.parts:
                    raise ValueError("artifact member escapes the export directory")
                target = root / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open("wb") as output:
                    offset = 0
                    while True:
                        part = rpc.call("ArtifactReadRequest", {
                            "artifact_id": artifact_id, "member": relative.as_posix(),
                            "offset": offset, "limit_bytes": 1 << 20,
                        })
                        output.write(bytes(part.get("bytes", [])))
                        if part.get("eof"):
                            break
                        next_offset = part.get("next_offset")
                        if next_offset is None or next_offset <= offset:
                            raise ValueError("artifact read did not advance")
                        offset = next_offset
        cursor = page.get("next_cursor")
        if not cursor:
            return


def collect(rpc, run_id, result, wired):
    page = rpc.call("RunLatestInvocationsRequest", {"run_id": run_id})
    cards = []
    for latest in page.get("records", []):
        card = latest["card"]
        key = "/".join(card["segments"])
        invocations = []
        cursor = None
        while True:
            request = {"run_id": run_id, "card": card}
            if cursor:
                request["cursor"] = cursor
            listed = rpc.call("RunInvocationsRequest", request)
            invocations += listed.get("records", [])
            cursor = listed.get("next_cursor")
            if not cursor:
                break
        for summary in invocations:
            number = summary.get("invocation", 1)
            folder = result / "cards" / key / str(number)
            folder.mkdir(parents=True, exist_ok=True)
            entry = {"card": key, "invocation": number, "state": summary.get("state"),
                     "duration_ms": summary.get("duration_ms"), "folder": str(folder.relative_to(result))}
            if summary.get("failure"):
                entry["failure"] = summary["failure"]
            record = rpc.call("RunRecordRequest", {"run_id": run_id, "card": card, "invocation": number}).get("record")
            if record:
                outputs = {}
                for value in record.get("outputs", []):
                    decoded = decode_value(value, folder)
                    if decoded is not None and (key, value["name"]) not in wired:
                        decoded["result"] = True
                    outputs[value["name"]] = decoded
                if outputs:
                    entry["outputs"] = outputs
            for stream, name in (("standard_output", "stdout.txt"), ("standard_error", "stderr.txt")):
                text = read_log(rpc, run_id, card, number, stream)
                if text:
                    (folder / name).write_text(text)
            cards.append(entry)
    return cards


def main():
    project, request_path, result = (Path(a) for a in sys.argv[1:4])
    work = Path(os.environ.get("PACK_WORK", "/work"))
    request = json.loads(request_path.read_text()) if request_path.is_file() else {}
    record_all(project)
    found = triggers(project)
    start = request.get("start") or (found[0] if found else None)
    if start is None:
        raise SystemExit("the Project has no trigger Card to start a run from")
    stage("core")
    began = time.monotonic()
    core, socket = start_core(project, work)
    rpc = Rpc(f"unix:{socket}", os.environ["TSUGUMI_SCHEMA"])
    summary = {"start": start, "triggers": found}
    try:
        rpc.call("SessionHelloRequest", {})
        revision = rpc.call("ProjectInspectRequest", {})["rev"]
        stage("run")
        accepted = rpc.call("RunRequest", {"base_rev": revision, "op_id": str(uuid.uuid4()), "origin": "pack",
                                          "card": {"segments": [start]},
                                          "max_concurrency": min(64, max(1, os.cpu_count() or 1))})
        run_id = accepted["run"]["run_id"]
        shown = {}
        while True:
            status = rpc.call("RunStatusRequest", {"run_id": run_id})
            latest = rpc.call("RunLatestInvocationsRequest", {"run_id": run_id})
            for record in latest.get("records", []):
                key = "/".join(record["card"]["segments"])
                state = record.get("state")
                if shown.get(key) != state:
                    shown[key] = state
                    print(f"PACK-CARD {key} {state}", flush=True)
            if status.get("state") in ENDED:
                break
            time.sleep(0.2)
        summary.update({"run_id": run_id, "state": status.get("state"),
                        "seconds": round(time.monotonic() - began, 3)})
        if status.get("failure"):
            summary["failure"] = status["failure"]
        stage("results")
        export_artifacts(rpc, run_id, result)
        summary["cards"] = collect(rpc, run_id, result, wired_outputs(project))
        summary["results"] = [{"card": c["card"], "invocation": c["invocation"], "port": port,
                               **{k: f"{c['folder']}/{v}" for k, v in value.items() if k in ("json", "file")}}
                              for c in summary["cards"] for port, value in c.get("outputs", {}).items()
                              if value and value.get("result")]
    finally:
        rpc.close()
        core.terminate()
        core.wait(timeout=30)
        shutil.copy(work / "tsugumid.log", result / "tsugumid.log")
    (result / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n")
    print(f"PACK-RESULT {summary['state']}", flush=True)
    return 0 if summary["state"] == "succeeded" else 1


if __name__ == "__main__":
    sys.exit(main())
