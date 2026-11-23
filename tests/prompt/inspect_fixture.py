"""Read-only stdio MCP replay for the two project-inspection prompt probes."""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = [
    {key: tool[key] for key in ("name", "description", "inputSchema")}
    for tool in json.loads((ROOT / "tests/fixtures/agent-tools.json").read_text())
]
# The one-Card fixture stores its initial value as a Nix literal.
SNAPSHOT = json.loads(Path(__file__).with_name("inspect_snapshot.json").read_text())


PARAM_PATH = "definitions/initial/params.nix"
PARAM_BYTES = b"{ value = 43; }\n"


def reply(request):
    method = request["method"]
    if method == "initialize":
        return {"protocolVersion": request["params"]["protocolVersion"], "capabilities": {"tools": {}},
                "serverInfo": {"name": "tsugumi-probe", "version": "1"}}
    if method == "tools/list":
        return {"tools": TOOLS}
    if method == "tools/call":
        params = request["params"]
        name = params["name"]
        arguments = params.get("arguments", {})
        if name == "project_inspect" and arguments == {}:
            result = SNAPSHOT
        elif name == "project_file_read" and arguments == {"path": PARAM_PATH}:
            source = next(s for s in SNAPSHOT["value"]["sources"] if s["path"] == PARAM_PATH)
            result = {"reply": "FileContent", "value": {
                "name": PARAM_PATH, "bytes": list(PARAM_BYTES), "hash": source["hash"],
                "size_bytes": len(PARAM_BYTES), "offset": 0, "eof": True,
            }}
        else:
            return {"isError": True, "content": [{"type": "text", "text": "Unexpected replay call."}]}
        return {"isError": False, "content": [{"type": "text", "text": json.dumps(result, separators=(",", ":"))}]}
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
