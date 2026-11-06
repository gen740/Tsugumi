"""Read-only stdio MCP replay for the two project-inspection prompt probes."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = [
    {key: tool[key] for key in ("name", "description", "inputSchema")}
    for tool in json.loads((ROOT / "tests/fixtures/agent-tools.json").read_text())
]
# Deliberately different from the bundled demo's 27. Only inspection reveals it.
SNAPSHOT = {
    "rev": "12",
    "project_json": json.dumps({"canvas": {"cards": [{"id": "initial"}], "connections": []}}),
    "definitions": [{"id": "initial", "card_json": json.dumps({
        "template": "input", "params": [{"name": "value", "type": "uint64", "data": 43}]
    })}],
}


def reply(request):
    method = request["method"]
    if method == "initialize":
        return {"protocolVersion": request["params"]["protocolVersion"], "capabilities": {"tools": {}},
                "serverInfo": {"name": "tsugumi-probe", "version": "1"}}
    if method == "tools/list":
        return {"tools": TOOLS}
    if method == "tools/call":
        inspect = request["params"]["name"] == "project_inspect"
        return {"isError": not inspect, "content": [{"type": "text",
                "text": json.dumps(SNAPSHOT) if inspect else "This replay only supports project_inspect."}]}
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
