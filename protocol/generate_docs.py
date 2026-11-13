"""Writes docs/Protocol.md from the project, Agent and Host schemas: every request of
RequestPayload with its doc comment and fields, then the notifications. The
schema stays the single source; run this after changing it.
--check only compares."""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FBS = ROOT / "protocol" / "tsugumi.fbs"
OUT = ROOT / "docs" / "Protocol.md"

GROUPS = [
    ("Connection", ["SessionHello"]),
    ("Projects and saving", ["ProjectOpen", "ProjectClose", "ProjectInspect", "ProjectSave", "ProjectReload",
                       "ProjectValidate", "ProjectImport", "ProjectShell", "ProjectCacheClear", "ProjectCacheUsage", "LayoutArrange", "LayoutUntangle", "OperationGet"]),
    ("Files", ["ProjectFileList", "ProjectFileStat", "ProjectFileRead", "ProjectFileFormat", "AttachedFolderRead", "ModuleRead"]),
    ("Card SDKs and types", ["CardSDKPrepare", "CardSDKStatus", "OutputTypes", "NixSourceResolve"]),
    ("Running", ["RunPlan", "Run", "RunCancel", "RunStatus"]),
    ("History and artifacts", ["RunHistory", "RunInvocations", "RunLatestInvocations", "RunRecord", "RunValueRead", "RunHistoryDelete",
                 "ArtifactList", "ArtifactRead", "LogRange", "RunEnvironments", "RunEnvironmentPaths"]),
    ("Views", ["ViewSetVisible", "ViewSave"]),
    ("Versions", ["VersionInit", "VersionMark", "VersionCheckout", "VersionGraph", "VersionCanvas", "VersionDiff", "VersionSummary"]),
]


def union_members(text: str, name: str) -> list[str]:
    found = re.search(rf"^union {name} \{{(.*?)^\}}", text, re.S | re.M)
    assert found, name
    return re.findall(r"^\s*(?:\w+\s*:\s*[\w.]+|(\w+))\s*=\s*\d+", found.group(1), re.M)


def tables(text: str) -> dict[str, dict]:
    """Each table with its /// comment and fields (name, type, required, comment)."""
    out: dict[str, dict] = {}
    comment: list[str] = []
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("///"):
            comment.append(line[3:].strip())
            continue
        if current is None:
            one = re.match(r"table (\w+) \{(.*)\}\s*$", line)
            many = re.match(r"table (\w+) \{\s*$", line)
            if one:
                body = one.group(2).strip()
                out[one.group(1)] = {"doc": " ".join(comment), "fields": fields_of(body)}
            elif many:
                current = many.group(1)
                out[current] = {"doc": " ".join(comment), "fields": []}
            comment = []
        elif line == "}":
            current = None
            comment = []
        else:
            for f in fields_of(line):
                f["doc"] = " ".join(comment)
                out[current]["fields"].append(f)
            if line:
                comment = []
    return out


def fields_of(body: str) -> list[dict]:
    found = []
    for m in re.finditer(r"(\w+)\s*:\s*(\[?[\w.]+\]?)\s*(=\s*[^;(]+)?\s*(\([^)]*\))?\s*;", body):
        attrs = m.group(4) or ""
        found.append({"name": m.group(1), "type": m.group(2).replace("Tsugumi.Data.", ""),
                      "required": "required" in attrs, "doc": ""})
    return found


def cell(s: str) -> str:
    return s.replace("|", "\\|").replace("\n", " ")


def service_docs() -> list[str]:
    out = ["", "## Host-side services", "",
           "`tsugumi-agentd` serves two separate FlatBuffers contracts. The Mac app uses the shared C ABI byte client; the Web host uses a Unix socket and generated TypeScript bindings. The browser retains its HTTP/SSE API; the host token never reaches it. `tsugumi-rpc` accepts project RPC only.", "",
           "| Contract | Root / identifier | WebSocket subprotocol | Principal |",
           "| --- | --- | --- | --- |",
           "| Agent (`agent.fbs`) | `Tsugumi.Agent.Message` / `TSA1` | `tsugumi.agent.v1` | User or external Agent |",
           "| Host (`host.fbs`) | `Tsugumi.Host.Message` / `TSH1` | `tsugumi.host.v1` | User only |", "",
           "Both use a little-endian uint32 length followed by a buffer on Unix, or one binary buffer per WebSocket message. The Host socket is the Agent socket path plus `.host`; its WebSocket listener uses a separate ephemeral port on the same address. `agent.ready` publishes `unix_socket`, `endpoint`, `host_socket` and `host_endpoint`. It is a private JSON discovery file, not an RPC envelope.", "",
           "`service_common.fbs` shares Hello, Failure, Acknowledgement and typed Provider configuration. Configure on Host provisions the global provider; Configure on Agent requires a conversation ID. The two roots reject each other's frames. All replies are typed; there is no `Result{json}`.", "",
           "JSON text is confined to model content (`ModelMessage.content_json`, `provider_content_json`), tool arguments/results (`ToolCallRequest.arguments_json`, `ToolRecord.arguments_json`, `result_json`, `Helper.result_json`, `Event.input_json`) and the open-ended JSON Schema of tool arguments (`Tool.input_schema_json`). Model provider blocks and tool payloads vary by provider/tool; service metadata, usage, permissions, credentials and provider configuration are typed.", ""]
    common = tables((ROOT / "protocol/service_common.fbs").read_text())
    for schema, title in [("agent", "Agent"), ("host", "Host")]:
        text = (ROOT / f"protocol/{schema}.fbs").read_text()
        definitions = tables(text)
        definitions.update(common)
        # These small service unions use implicit enum values and qualified aliases.
        out += [f"### {title} requests", "", "| Request | Fields |", "| --- | --- |"]
        body = re.search(r"union RequestPayload \{(.*?)\}", text, re.S).group(1)
        for item in body.split(","):
            name = item.strip().split(":")[0]
            fields = definitions[name]["fields"]
            out.append(f"| `{name}` | " + ", ".join(f"`{f['name']}:{f['type']}`" + (" (required)" if f["required"] else "") for f in fields) + " |")
        body = re.search(r"union ReplyPayload \{(.*?)\}", text, re.S).group(1)
        out += ["", "Replies: " + ", ".join("`" + item.strip().split(":")[0] + "`" for item in body.split(",")) + ".", ""]
    return out


def render() -> str:
    text = FBS.read_text()
    all_tables = tables(text)
    requests = [m[: -len("Request")] for m in union_members(text, "RequestPayload")]
    grouped = [n for _, names in GROUPS for n in names]
    assert sorted(grouped) == sorted(requests), set(grouped) ^ set(requests)

    out = [
        "# The RPC contract",
        "",
        "<!-- Generated by protocol/generate_docs.py from protocol/*.fbs: do not edit. -->",
        "",
        "The RPCs by which the UI, the Web and the Agent talk to `tsugumid`, generated from the doc comments of [protocol/tsugumi.fbs](../protocol/tsugumi.fbs).",
        "",
        "- **Format**: one message is one FlatBuffer (root `Tsugumi.RPC.Message`, file identifier `TSG1`). The WebSocket subprotocol is `tsugumi.rpc.v1`; on a Unix socket, a 4-byte length (little endian) is followed by the body.",
        "- **Failure**: any request may be answered with `Failure`.",
        "- **Requests that change something**: carry an `op_id`, and a resend with the same `op_id` is applied only once. If a response is lost, check with `OperationGet`.",
        "- **The Agent's tools**: made from the same schema (`tsugumi.bfbs`) ([Features](features.md#9-agent)).",
        "",
        f"There are {len(requests)} requests.",
        "",
    ]
    for title, names in GROUPS:
        out += [f"## {title}", "", "| Request | Response | Description |", "| --- | --- | --- |"]
        for n in names:
            doc = all_tables[n + "Request"]["doc"]
            m = re.match(r"->\s*([^.]+)\.?\s*(.*)", doc)
            reply, rest = (m.group(1).strip(), m.group(2).strip()) if m else ("", doc)
            out.append(f"| [`{n}`](#{n.lower()}) | {cell(reply)} | {cell(rest)} |")
        out.append("")
        for n in names:
            t = all_tables[n + "Request"]
            out += [f"### {n}", ""]
            if t["fields"]:
                out += ["| Field | Type | Required | Description |", "| --- | --- | --- | --- |"]
                for f in t["fields"]:
                    out.append(f"| `{f['name']}` | `{cell(f['type'])}` | {'yes' if f['required'] else ''} | {cell(f['doc'])} |")
            else:
                out.append("No fields.")
            out.append("")

    out += ["## Notifications", "", "Sent from `tsugumid` to connected clients.", ""]
    for n in union_members(text, "NotificationPayload"):
        out.append(f"- `{n}`")
    out += ["", "### Run events", "", "The `value` inside a `RunEvent` has one of these types.", ""]
    for n in union_members(text, "RunEventValue"):
        out.append(f"- `{n}`")
    out += service_docs()
    return "\n".join(out).rstrip() + "\n"


if __name__ == "__main__":
    made = render()
    if "--check" in sys.argv:
        sys.exit(0 if OUT.exists() and OUT.read_text() == made else 1)
    OUT.write_text(made)
