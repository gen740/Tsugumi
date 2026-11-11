"""One opt-in live Codex turn. Report the reply, tool calls and usage for manual paired scoring."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from inspect_fixture import SNAPSHOT, TOOLS

ROOT = Path(__file__).resolve().parents[2]
CASES = {
    "concept": "Tsugumi の Draft はそのまま実行できますか？ 一文で教えてください。",
    "project": "このプロジェクトの initial Card に設定されている初期値はいくつですか？ 一文で教えてください。",
}


def received_snapshot(result):
    for block in result.get("content", []):
        if block.get("type") == "text":
            try:
                if json.loads(block["text"]) == SNAPSHOT:
                    return True
            except (KeyError, ValueError):
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
            if call["server"] == "tsugumi" and call["tool"] == "project_inspect":
                call["snapshot_received"] = received_snapshot(result)
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
        if any(c.get("server") != "tsugumi" or c["tool"] != "project_inspect" or c["arguments"] != {} for c in calls):
            return dict(report, outcome="fail", reason="Unexpected tool, server, or inspection arguments")
        if any(c["status"] != "completed" or c["error"] or not c["snapshot_received"] for c in calls):
            return dict(report, outcome="invalid", reason="The requested fixture inspection did not succeed")
        if not calls:
            return dict(report, outcome="fail", reason="Project-specific answer has no observed evidence")
        report["extra_inspections"] = len(calls) - 1
    if not any(text.strip() for text in answers):
        return dict(report, outcome="fail", reason="No assistant reply")
    return dict(report, outcome="needs_review", reason="Check the reply and any unclassified items against the rubric")


def run_trial(question, prompt, fixture, model, codex, approved_tools=("project_inspect",)):
    """Run one isolated read-only turn; keep raw events for independent scoring."""
    note = json.loads((ROOT / "agent/Shared/agent.json").read_text())["guide"]["chatNote"]
    with tempfile.TemporaryDirectory() as tmp:
        cwd = Path(tmp) / "empty"
        cwd.mkdir()
        instructions = Path(tmp) / "instructions.md"
        instructions.write_text(prompt.read_text().rstrip() + "\n\n" + note)
        prompt_hash = hashlib.sha256(instructions.read_bytes()).hexdigest()
        command = [
            codex, "exec", "--ignore-user-config", "--skip-git-repo-check", "--ephemeral",
            "-s", "read-only", "-C", str(cwd), "-m", model, "--json",
            "--disable", "shell_tool", "--disable", "multi_agent", "--disable", "plugins",
            "-c", 'model_reasoning_effort="low"', "-c", 'web_search="disabled"',
            "-c", f"model_instructions_file={json.dumps(str(instructions))}",
            "-c", f"mcp_servers.tsugumi.command={json.dumps(sys.executable)}",
            "-c", f"mcp_servers.tsugumi.args={json.dumps([str(fixture)])}",
            "-c", "mcp_servers.tsugumi.required=true",
        ]
        for tool in approved_tools:
            command += ["-c", f'mcp_servers.tsugumi.tools.{tool}.approval_mode="approve"']
        command.append(question)
        version = None
        try:
            version = subprocess.check_output([codex, "--version"], text=True, timeout=10).strip()
            run = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True, timeout=120)
            stdout, error = run.stdout, f"CLI exited {run.returncode}" if run.returncode else None
        except subprocess.TimeoutExpired as e:
            stdout, error = (e.stdout or b"").decode(errors="replace"), "CLI timed out"
        except (OSError, subprocess.CalledProcessError) as e:
            stdout, error = "", str(e)
    return stdout, error, version, prompt_hash


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", choices=CASES)
    parser.add_argument("--prompt", type=Path, default=ROOT / "agent/Shared/system-prompt.md")
    parser.add_argument("--model", default="gpt-6.1-sol")
    parser.add_argument("--codex", default=os.environ.get("CODEX_BIN", "codex"))
    args = parser.parse_args()
    stdout, error, version, prompt_hash = run_trial(
        CASES[args.case], args.prompt, Path(__file__).with_name("inspect_fixture.py").resolve(), args.model, args.codex)
    report = summarize(args.case, stdout, error)
    fixture_data = json.dumps([TOOLS, SNAPSHOT], sort_keys=True).encode()
    print(json.dumps({"case": args.case, "model": args.model, "effort": "low", "codex_version": version,
                      "instructions_sha256": prompt_hash, "fixture_sha256": hashlib.sha256(fixture_data).hexdigest(),
                      **report}, ensure_ascii=False, indent=2))
    return {"needs_review": 0, "fail": 1, "invalid": 2}[report["outcome"]]


if __name__ == "__main__":
    sys.exit(main())
