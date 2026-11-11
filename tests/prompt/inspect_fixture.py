"""Read-only stdio MCP replay for the two project-inspection prompt probes."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = [
    {key: tool[key] for key in ("name", "description", "inputSchema")}
    for tool in json.loads((ROOT / "tests/fixtures/agent-tools.json").read_text())
]
# Captured through the real MCP server and tsugumid for a one-Card project
# with the Collatz initial definition, set to 43 instead of the demo's 27.
SNAPSHOT = json.loads(Path(__file__).with_name("inspect_snapshot.json").read_text())


def reply(request):
    method = request["method"]
    if method == "initialize":
        return {"protocolVersion": request["params"]["protocolVersion"], "capabilities": {"tools": {}},
                "serverInfo": {"name": "tsugumi-probe", "version": "1"}}
    if method == "tools/list":
        return {"tools": TOOLS}
    if method == "tools/call":
        params = request["params"]
        if params["name"] != "project_inspect":
            return {"isError": True, "content": [{"type": "text", "text": "This replay only supports project_inspect."}]}
        if params.get("arguments", {}) != {}:
            return {"isError": True, "content": [{"type": "text", "text": "project_inspect takes no arguments."}]}
        return {"isError": False, "content": [{"type": "text", "text": json.dumps(SNAPSHOT, separators=(",", ":"))}]}
    if method == "ping":
        return {}
    raise KeyError(method)


if __name__ == "__main__":
    for line in sys.stdin:
        request = json.loads(line)
        if "id" not in request:
            continue
        response = {"jsonrpc": "2.0", "id": request["id"]}
        try:
            response["result"] = reply(request)
        except KeyError:
            response["error"] = {"code": -32601, "message": "Unknown request"}
        print(json.dumps(response), flush=True)
