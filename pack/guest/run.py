"""Run an exported Project once, inside the pack guest, and write its results.

Called by the guest's /init in run mode. The Project is a copy of what the
author exported; the recipient's request (parameter values) is applied to that
copy before tsugumid opens it, so the run is an ordinary run of the Workflow
from its trigger. Nothing here schedules Cards: tsugumid does, through the
same RPC the apps use (tsugumi-rpc, one JSON request per line).

    python3 run.py PROJECT REQUEST RESULT

PROJECT  writable copy of the exported Project
REQUEST  request.json from the Mac: {"start": "<trigger>", "params": {"<card>": {"<name>": value}},
         "files": {"<input card>": "<file name in inputs/<card>/ beside request.json>"}}
RESULT   folder for summary.json and cards/<card>/<invocation>/...

The copy records every Code Card's inputs and outputs (recordIO), whatever the
author chose: the recipient gets values back, not only displays. recordIO is
a setting, so this changes no build.
"""

import json
import os
import shutil
import subprocess
import sys
import time
import uuid
from pathlib import Path

ENDED = {"succeeded", "failed", "cancelled"}
# Card kinds that expose editable values in the exported app.
SETTABLE = {"code", "input"}


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


def definition_of(project, card):
    path = project / "definitions" / card / "_card.json"
    if not path.is_file():
        raise SystemExit(f"no Card {card} in this Project")
    return path, json.loads(path.read_text())


def apply_request(project, request, inputs):
    """Set the recipient's parameter values and files in the Cards' _card.json."""
    for card, name in request.get("files", {}).items():
        path, definition = definition_of(project, card)
        values = [p for p in definition.get("params", []) if p["name"] == "value"]
        if definition.get("type") != "input" or not values:
            raise SystemExit(f"{card} is not an input Card with a file")
        # An input Card's file is a source of its own folder: the core copies
        # it into the run from there.
        source = inputs / card / name
        target = project / "definitions" / card / "recipient" / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        values[0]["data"] = f"recipient/{name}"
        path.write_text(json.dumps(definition, indent=2, ensure_ascii=False) + "\n")
    for card, values in request.get("params", {}).items():
        path, definition = definition_of(project, card)
        if definition.get("type") not in SETTABLE:
            raise SystemExit(f"{card} is a {definition.get('type')} Card: its parameters shape the Workflow")
        params = {p["name"]: p for p in definition.get("params", [])}
        for name, value in values.items():
            if name not in params:
                raise SystemExit(f"Card {card} has no parameter {name} (it has {', '.join(params) or 'none'})")
            if params[name].get("nix"):
                raise SystemExit(f"{card}.{name} is fixed at export: it changes the Card's build")
            params[name]["data"] = value
        path.write_text(json.dumps(definition, indent=2, ensure_ascii=False) + "\n")


def record_all(project):
    """recordIO on for every Code Card of the copy."""
    for path in (project / "definitions").glob("*/_card.json"):
        definition = json.loads(path.read_text())
        if definition.get("type") == "code" and not definition.get("recordIO"):
            definition["recordIO"] = True
            path.write_text(json.dumps(definition, indent=2, ensure_ascii=False) + "\n")


def wired_outputs(project):
    """(card, port) pairs that feed another Card: the rest are the Workflow's results."""
    connections = json.loads((project / "tsugumi.json").read_text()).get("canvas", {}).get("connections", [])
    return {(c["from"]["card"], c["from"]["port"]) for c in connections}


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
            return process, socket, json.loads(ready.read_text())["token"]
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
        return None
    raw = folder / f"{name}.bin"
    raw.write_bytes(data)
    entry = {"bytes": f"{name}.bin"}
    if value.get("kind") in ("File", "Directory"):
        # The content itself, from where the core keeps it.
        kept = value.get("file_path")
        if kept and os.path.exists(kept):
            target = folder / name / os.path.basename(kept.rstrip("/"))
            target.parent.mkdir(parents=True, exist_ok=True)
            if os.path.isdir(kept):
                shutil.copytree(kept, target, symlinks=True)
            else:
                shutil.copyfile(kept, target)
            entry["file"] = str(target.relative_to(folder))
        else:
            entry["file_missing"] = True
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
        if "file" in entry:
            # The path inside the guest means nothing outside it: name the copy.
            decoded = json.loads(produced.read_text())
            if isinstance(decoded, dict) and isinstance(decoded.get("value"), str):
                decoded["value"] = entry["file"]
                produced.write_text(json.dumps(decoded, indent=2, ensure_ascii=False) + "\n")
    return entry


def write_display(display, folder):
    """The Card's display as a plain file: no viewer in this path."""
    kind = display.get("value_type")
    value = display.get("value", {})
    if kind == "DisplayText":
        name = "display.md" if value.get("markdown") else "display.txt"
        (folder / name).write_text(value.get("text", ""))
    elif kind == "DisplayImage":
        extension = "jpg" if value.get("media_type") == "image/jpeg" else "png"
        name = f"display.{extension}"
        (folder / name).write_bytes(bytes(value.get("data", [])))
    elif kind == "DisplayScalar":
        name = "display.json"
        (folder / name).write_text(json.dumps(value) + "\n")
    elif kind == "DisplayTable":
        name = "display.tsv"
        rows = [value.get("columns", [])] + [r.get("cells", []) for r in value.get("rows", [])]
        (folder / name).write_text("".join("\t".join(r) + "\n" for r in rows))
    elif kind:
        name = "display.json"
        (folder / name).write_text(json.dumps({"kind": kind}) + "\n")
    else:
        return None
    return name


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
                if record.get("display"):
                    entry["display"] = write_display(record["display"], folder)
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
    apply_request(project, request, request_path.parent / "inputs")
    record_all(project)
    found = triggers(project)
    start = request.get("start") or (found[0] if found else None)
    if start is None:
        raise SystemExit("the Project has no trigger Card to start a run from")
    stage("core")
    began = time.monotonic()
    core, socket, token = start_core(project, work)
    rpc = Rpc(f"unix:{socket}", os.environ["TSUGUMI_SCHEMA"])
    summary = {"start": start, "triggers": found, "params": request.get("params", {})}
    try:
        rpc.call("SessionHelloRequest", {"token": token})
        snapshot = rpc.call("ProjectInspectRequest", {})
        stage("run")
        accepted = rpc.call("RunRequest", {"base_rev": snapshot["rev"], "op_id": str(uuid.uuid4()), "origin": "pack",
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
