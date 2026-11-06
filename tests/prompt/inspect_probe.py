"""One opt-in live Codex turn. Report the reply, tool calls and usage for manual paired scoring."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CASES = {
    "concept": "Tsugumi の Draft はそのまま実行できますか？ 一文で教えてください。",
    "project": "このプロジェクトの initial Card に設定されている初期値はいくつですか？ 一文で教えてください。",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", choices=CASES)
    parser.add_argument("--prompt", type=Path, default=ROOT / "agent/Shared/system-prompt.md")
    parser.add_argument("--model", default="gpt-6.1-sol")
    parser.add_argument("--codex", default=os.environ.get("CODEX_BIN", "codex"))
    args = parser.parse_args()
    note = json.loads((ROOT / "agent/Shared/agent.json").read_text())["guide"]["chatNote"]
    with tempfile.TemporaryDirectory() as tmp:
        cwd = Path(tmp) / "empty"
        cwd.mkdir()
        instructions = Path(tmp) / "instructions.md"
        instructions.write_text(args.prompt.read_text().rstrip() + "\n\n" + note)
        fixture = Path(__file__).with_name("inspect_fixture.py").resolve()
        command = [
            args.codex, "exec", "--ignore-user-config", "--skip-git-repo-check", "--ephemeral",
            "-s", "read-only", "-C", str(cwd), "-m", args.model, "--json",
            "--disable", "shell_tool", "--disable", "multi_agent", "--disable", "plugins",
            "-c", 'model_reasoning_effort="low"', "-c", 'web_search="disabled"',
            "-c", f"model_instructions_file={json.dumps(str(instructions))}",
            "-c", f"mcp_servers.tsugumi.command={json.dumps(sys.executable)}",
            "-c", f"mcp_servers.tsugumi.args={json.dumps([str(fixture)])}",
            "-c", 'mcp_servers.tsugumi.tools.project_inspect.approval_mode="approve"',
            CASES[args.case],
        ]
        run = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True, timeout=120, check=True)
    events = [json.loads(line) for line in run.stdout.splitlines()]
    items = [event["item"] for event in events if event["type"] == "item.completed"]
    completed = next((e for e in events if e["type"] == "turn.completed"), None)
    if completed is None:
        raise SystemExit("No completed turn. Exclude this trial from the comparison.")
    calls = [{"tool": item.get("tool", item["type"]), "status": item.get("status"),
              "error": item.get("error") or (item.get("result") or {}).get("isError", False)}
             for item in items if item["type"] not in ("agent_message", "reasoning")]
    answers = [item["text"] for item in items if item["type"] == "agent_message"]
    print(json.dumps({"case": args.case, "model": args.model, "calls": calls,
                      "reply": answers, "usage": completed["usage"]}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
