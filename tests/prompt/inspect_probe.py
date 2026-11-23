"""One opt-in live Codex turn. Report the reply, tool calls and usage for manual paired scoring."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from inspect_fixture import PARAM_BYTES, PARAM_PATH, TOOLS

ROOT = Path(__file__).resolve().parents[2]
CASES = {
    "concept": "Tsugumi の Draft はそのまま実行できますか？ 一文で教えてください。",
    "project": "このプロジェクトの initial Card に設定されている初期値はいくつですか？ 一文で教えてください。",
}


def received_parameter(result):
    for block in result.get("content", []):
        if block.get("type") != "text":
            continue
        try:
            response = json.loads(block["text"])
            value = response["value"]
            if (response["reply"] == "FileContent" and value["name"] == PARAM_PATH
                    and value["eof"] and bytes(value["bytes"]) == PARAM_BYTES):
                return True
        except (KeyError, ValueError, TypeError):
            continue
    return False


def summarize(case, stdout, process_error=None):
    """Grade the observable tool path. Leave the meaning of the reply to a human."""
    items = {}  # Keep attempted calls even if the CLI exits before item.completed.
    answers = []
    usage = None
    errors = []
    for line in stdout.splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            process_error = "Incomplete or malformed CLI event stream"
            break
        kind = event["type"]
        if kind in ("item.started", "item.updated", "item.completed"):
            item = event["item"]
            items[item["id"]] = item
            if kind == "item.completed" and item["type"] == "agent_message":
                answers.append(item["text"])
        elif kind == "turn.completed":
            usage = event.get("usage")
        elif kind in ("error", "turn.failed"):
            errors.append(event.get("message") or event.get("error"))
            if kind == "turn.failed":
                process_error = "CLI turn failed"

    calls = []
    other = []
    for item in items.values():
        kind = item["type"]
        if kind == "mcp_tool_call":
            result = item.get("result") or {}
            call = {"server": item["server"], "tool": item["tool"], "arguments": item.get("arguments"),
                    "status": item.get("status"), "error": item.get("error") or result.get("isError", False)}
            if call["server"] == "tsugumi" and call["tool"] == "project_file_read":
                call["parameter_received"] = received_parameter(result)
            calls.append(call)
        elif kind in ("command_execution", "file_change", "web_search", "collab_tool_call", "dynamic_tool_call"):
            calls.append({"tool": kind, "status": item.get("status")})
        elif kind not in ("agent_message", "reasoning"):
            other.append(kind)
    report = {"calls": calls, "reply": answers, "usage": usage, "other_items": other, "cli_errors": errors}
    if process_error or usage is None:
        return dict(report, outcome="invalid", reason=process_error or "Missing completed turn or usage")
    if case == "concept" and calls:
        return dict(report, outcome="fail", reason="General question attempted a tool")
    if case == "project":
        allowed = {"project_file_read": {"path": PARAM_PATH}, "project_inspect": {}}
        if any(c.get("server") != "tsugumi" or c["tool"] not in allowed
               or c["arguments"] != allowed[c["tool"]] for c in calls):
            return dict(report, outcome="fail", reason="Unexpected tool, server, or arguments")
        if not calls:
            return dict(report, outcome="fail", reason="Project-specific answer has no observed evidence")
        reads = [c for c in calls if c["tool"] == "project_file_read"]
        successes = sum(c["status"] == "completed" and not c["error"] and c["parameter_received"]
                        for c in reads)
        report["failed_reads"] = len(reads) - successes
        report["extra_reads"] = max(0, len(reads) - 1)
        if not successes:
            return dict(report, outcome="invalid", reason="The params.nix literal was not read")
    if not any(text.strip() for text in answers):
        return dict(report, outcome="fail", reason="No assistant reply")
    return dict(report, outcome="needs_review", reason="Check the reply and any unclassified items against the rubric")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", choices=CASES)
    parser.add_argument("--prompt", type=Path, default=ROOT / "hostd/agent/Shared/system-prompt.md")
    parser.add_argument("--model", default="gpt-6.1-sol")
    parser.add_argument("--codex", default=os.environ.get("CODEX_BIN", "codex"))
    args = parser.parse_args()
    note = json.loads((ROOT / "hostd/agent/Shared/agent.json").read_text())["guide"]["chatNote"]
    with tempfile.TemporaryDirectory() as tmp:
        cwd = Path(tmp) / "empty"
        cwd.mkdir()
        instructions = Path(tmp) / "instructions.md"
        instructions.write_text(args.prompt.read_text().rstrip() + "\n\n" + note)
        prompt_hash = hashlib.sha256(instructions.read_bytes()).hexdigest()
        fixture = Path(__file__).with_name("inspect_fixture.py").resolve()
        command = [
            args.codex, "exec", "--ignore-user-config", "--skip-git-repo-check", "--ephemeral",
            "-s", "read-only", "-C", str(cwd), "-m", args.model, "--json",
            "--disable", "shell_tool", "--disable", "multi_agent", "--disable", "plugins",
            "-c", 'model_reasoning_effort="low"', "-c", 'web_search="disabled"',
            "-c", f"model_instructions_file={json.dumps(str(instructions))}",
            "-c", f"mcp_servers.tsugumi.command={json.dumps(sys.executable)}",
            "-c", f"mcp_servers.tsugumi.args={json.dumps([str(fixture)])}",
            "-c", "mcp_servers.tsugumi.required=true",
            "-c", 'mcp_servers.tsugumi.tools.project_inspect.approval_mode="approve"',
            CASES[args.case],
        ]
        version = None
        try:
            version = subprocess.check_output([args.codex, "--version"], text=True, timeout=10).strip()
            run = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True, timeout=120)
            report = summarize(args.case, run.stdout, f"CLI exited {run.returncode}" if run.returncode else None)
        except subprocess.TimeoutExpired as e:
            partial = (e.stdout or b"").decode(errors="replace")
            report = summarize(args.case, partial, "CLI timed out")
        except (OSError, subprocess.CalledProcessError) as e:
            report = summarize(args.case, "", str(e))
    fixture_data = json.dumps([TOOLS, SNAPSHOT], sort_keys=True).encode()
    print(json.dumps({"case": args.case, "model": args.model, "effort": "low", "codex_version": version,
                      "instructions_sha256": prompt_hash, "fixture_sha256": hashlib.sha256(fixture_data).hexdigest(),
                      **report}, ensure_ascii=False, indent=2))
    return {"needs_review": 0, "fail": 1, "invalid": 2}[report["outcome"]]


if __name__ == "__main__":
    sys.exit(main())
