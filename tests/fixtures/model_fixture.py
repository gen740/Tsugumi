#!/usr/bin/env python3
"""Local model provider fixture (Anthropic Messages API shape, SSE streaming).

It replaces only the model connection. Every decision is derived from the
conversation it receives, so the Agent can disconnect, restart and resume.
Scripted task, triggered by a user message like:

    set <card>.<param> to <value> and run, then report <card>

Steps: project_inspect -> project_save (SetProject with the param changed)
-> run (from the root trigger) -> run_status until terminal -> run_record of
the reported card -> a final explanation.

A second scripted task implements Drafts ("Implement these Draft Cards ...
- <path> (definition <id>)"): project_inspect -> one project_save with
PutDefinition/PutSource per Draft -> project_validate -> run -> report.

Options:
  --slow SECONDS   delay between streamed chunks (cancellation tests)
  --drop-once      drop the first response mid-stream (retry tests)
"""

from __future__ import annotations

import argparse
import json
import re
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TASK = re.compile(r"set (\S+?)\.(\S+) to (.+?) and run(?:, then report (\S+))?", re.I)
TERMINAL = {"succeeded", "failed", "cancelled"}


def blocks(msg):
    c = msg.get("content")
    return [{"type": "text", "text": c}] if isinstance(c, str) else (c or [])


def last_task(messages):
    """The latest task request and the index of its message."""
    for i in range(len(messages) - 1, -1, -1):
        m = messages[i]
        if m["role"] != "user":
            continue
        for b in blocks(m):
            if b.get("type") == "text":
                t = TASK.search(b["text"])
                if t:
                    return t, i
    return None, -1


def tool_results(messages):
    """(tool name, parsed result) pairs in order."""
    names = {}
    out = []
    for m in messages:
        for b in blocks(m):
            if b.get("type") == "tool_use":
                names[b["id"]] = b["name"]
            elif b.get("type") == "tool_result":
                try:
                    content = json.loads(b["content"])
                except (ValueError, TypeError):
                    content = {"error": b["content"]}
                out.append((names.get(b["tool_use_id"], "?"), content, b.get("is_error", False)))
    return out


def trigger_of(inspect_value):
    """The Card whose own definition is the trigger template."""
    for d in inspect_value.get("definitions", []):
        if d.get("card_json") and json.loads(d["card_json"]).get("template") == "trigger":
            return d["id"]
    return "begin"


DRAFTS = re.compile(r"Implement these Draft Cards", re.I)
DRAFT_LINE = re.compile(r"^- (\S+) \(definition (\S+)\)", re.M)


def last_draft_task(messages):
    for i in range(len(messages) - 1, -1, -1):
        m = messages[i]
        if m["role"] != "user":
            continue
        for b in blocks(m):
            if b.get("type") == "text" and DRAFTS.search(b["text"]):
                return b["text"], i
    return None, -1


def implement(definition, meta):
    """A bash Card that fulfils a Draft: its planned outputs carry a short text."""
    card = {k: v for k, v in definition.items() if k != "template"}
    card["language"] = "bash"
    outs = [o["name"] for o in (meta or {}).get("outputs", [])] or ["out"]
    fbs = "namespace generated;\ntable Text { value:string; }\ntable Outputs { " + \
        " ".join(f"{o}:Text;" for o in outs) + " }\nroot_type Outputs;\n"
    payload = json.dumps({o: {"value": "done"} for o in outs})
    nix = "{ pkgs, inputs, params }:\n{\n  run = ''\n    echo '" + payload + "' > \"$TSUGUMI_OUTPUT\"\n  '';\n}\n"
    return card, fbs, nix


def decide_drafts(messages, text, start):
    results = tool_results(messages[start + 1:])
    if not results:
        return ("tool", "project_inspect", {})
    name, res, is_error = results[-1]
    value = res.get("value", {})
    if name == "project_inspect":
        defs = {d["id"]: d for d in value.get("definitions", [])}
        changes = []
        for _, def_id in DRAFT_LINE.findall(text):
            d = defs[def_id]
            meta = json.loads(d["meta_json"]) if d.get("meta_json") else {}
            card, fbs, nix = implement(json.loads(d["card_json"]), meta)
            # The intent and the port purposes stay in _meta.json.
            changes.append({"value_type": "PutDefinition", "value": {
                "id": def_id, "card_json": json.dumps(card), "meta_json": json.dumps(meta)}})
            for rel, body in (("_outputs.fbs", fbs), ("default.nix", nix)):
                changes.append({"value_type": "PutSource", "value": {
                    "path": f"definitions/{def_id}/{rel}", "bytes": body}})
        return ("tool", "project_save", {"base_rev": str(value["rev"]), "changes": changes})
    if name == "project_save":
        if is_error:
            return ("text", f"Saving failed: {value.get('diagnostic')}. The Drafts are unchanged.")
        return ("tool", "project_validate", {"at_rev": str(value["rev"])})
    if name == "project_validate":
        problems = [d.get("message") for d in value.get("diagnostics", []) if d.get("severity") != "warning"]
        inspect = next(r for n, r, _ in tool_results(messages[start + 1:]) if n == "project_inspect")
        trigger = trigger_of(inspect["value"])
        rev = next(r for n, r, _ in reversed(results) if n == "project_save")["value"]["rev"]
        return ("tool", "run", {"base_rev": str(rev), "card": {"segments": [trigger]}}) if True else ("text", str(problems))
    if name in ("run", "run_status"):
        if is_error:
            return ("text", f"Implemented and validated; the run could not start: {value.get('diagnostic')}")
        status = value.get("run", value)
        if status.get("state") not in TERMINAL:
            time.sleep(0.2)
            return ("tool", "run_status", {"run_id": status.get("run_id")})
        failure = (status.get("failure") or {}).get("diagnostic", "")
        return ("text", f"Implemented the Drafts and ran the workflow: {status.get('state')} {failure}")
    return ("text", "Done.")


def decide(messages):
    """Returns ("tool", name, input) or ("text", message)."""
    draft_text, draft_start = last_draft_task(messages)
    task, start = last_task(messages)
    if draft_text is not None and draft_start > start:
        return decide_drafts(messages, draft_text, draft_start)
    if not task:
        return ("text", "I can change a Card setting and run the graph: say 'set <card>.<param> to <value> and run'.")
    card_path, param, value, report = task.group(1).split("/"), task.group(2), task.group(3).strip(), task.group(4)
    # Only results after the latest task request count.
    results = tool_results(messages[start + 1:])
    if not results:
        return ("tool", "project_inspect", {})
    name, res, is_error = results[-1]
    value_obj = res.get("value", {})
    if name == "project_inspect":
        # A Card's settings are its own definition (definitions/<id>/_card.json).
        d = next(x for x in value_obj["definitions"] if x["id"] == card_path[-1])
        card = json.loads(d["card_json"])
        try:
            v = json.loads(value)
        except ValueError:
            v = value
        next(p for p in card["params"] if p["name"] == param)["data"] = v
        change = {"id": d["id"], "card_json": json.dumps(card)}
        if d.get("meta_json"):
            change["meta_json"] = d["meta_json"]
        return ("tool", "project_save", {
            "base_rev": str(value_obj["rev"]),
            "changes": [{"value_type": "PutDefinition", "value": change}],
        })
    if name == "project_save":
        if is_error:
            if value_obj.get("kind") == "revision_conflict":
                return ("tool", "project_inspect", {})
            return ("text", f"Saving failed: {value_obj.get('diagnostic')}")
        inspect = next(r for n, r, _ in reversed(results) if n == "project_inspect")
        trigger = trigger_of(inspect["value"])
        return ("tool", "run", {"base_rev": str(value_obj["rev"]), "card": {"segments": [trigger]}})
    if name in ("run", "run_status"):
        if is_error:
            return ("text", f"The run could not start: {value_obj.get('diagnostic')}")
        status = value_obj.get("run", value_obj)
        run_id = status.get("run_id")
        if status.get("state") not in TERMINAL:
            time.sleep(0.2)
            return ("tool", "run_status", {"run_id": run_id})
        if report:
            return ("tool", "run_record", {"run_id": run_id, "card": {"segments": report.split("/")}})
        return ("text", f"Run {run_id} finished: {status.get('state')}.")
    if name == "run_record":
        st = value_obj.get("run", {}).get("status", {})
        record = value_obj.get("record") or {}
        display = (record.get("display") or {}).get("value", {})
        shown = display.get("text") or json.dumps(display)[:200]
        return ("text", f"I set {'/'.join(card_path)}.{param} to {value} and ran the graph. "
                        f"Run {st.get('run_id')} {st.get('state')}; {report} shows: {shown}")
    return ("text", "Done.")


class Handler(BaseHTTPRequestHandler):
    server_version = "TsugumiModelFixture/1"
    slow = 0.0
    drop_once = False
    dropped = False
    lock = threading.Lock()

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path.rstrip("/") == "/v1/models":
            self._json({"data": [{"id": "fixture", "type": "model"}]})
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path == "/v1/responses":
            self._responses()
            return
        if self.path != "/v1/messages":
            self.send_error(404)
            return
        body = json.loads(self.rfile.read(int(self.headers["content-length"])))
        if body.get("system", "").startswith("You summarize"):
            decision = ("text", "Earlier: the user asked for a setting change and a run.")
        else:
            decision = decide(body["messages"])
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.end_headers()
        usage_in = len(json.dumps(body["messages"])) // 4
        self._event("message_start", {"type": "message_start", "message": {"usage": {"input_tokens": usage_in}}})
        with Handler.lock:
            drop = Handler.drop_once and not Handler.dropped
            Handler.dropped = Handler.dropped or drop
        if drop:
            # Cut the connection after the first block started.
            self._event("content_block_start", {"type": "content_block_start", "index": 0,
                                                 "content_block": {"type": "text", "text": ""}})
            self.connection.shutdown(2)
            return
        if decision[0] == "text":
            self._event("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}})
            words = decision[1].split(" ")
            for i, w in enumerate(words):
                self._event("content_block_delta", {"type": "content_block_delta", "index": 0,
                                                    "delta": {"type": "text_delta", "text": w + (" " if i + 1 < len(words) else "")}})
            self._event("content_block_stop", {"type": "content_block_stop", "index": 0})
            stop = "end_turn"
        else:
            _, name, args = decision
            self._event("content_block_start", {"type": "content_block_start", "index": 0,
                                                 "content_block": {"type": "tool_use", "id": "toolu_" + uuid.uuid4().hex[:12], "name": name, "input": {}}})
            text = json.dumps(args)
            for i in range(0, len(text), 24):
                self._event("content_block_delta", {"type": "content_block_delta", "index": 0,
                                                    "delta": {"type": "input_json_delta", "partial_json": text[i:i + 24]}})
            self._event("content_block_stop", {"type": "content_block_stop", "index": 0})
            stop = "tool_use"
        self._event("message_delta", {"type": "message_delta", "delta": {"stop_reason": stop}, "usage": {"output_tokens": 20}})
        self._event("message_stop", {"type": "message_stop"})

    def _responses(self):
        """The same decisions over the OpenAI Responses API wire format."""
        body = json.loads(self.rfile.read(int(self.headers["content-length"])))
        if self.headers.get("authorization") != "Bearer fixture-key":
            self.send_error(401)
            return
        messages = []
        for item in body.get("input", []):
            if item.get("type") == "function_call":
                block = {"type": "tool_use", "id": item["call_id"], "name": item["name"], "input": json.loads(item["arguments"] or "{}")}
                role = "assistant"
            elif item.get("type") == "function_call_output":
                block = {"type": "tool_result", "tool_use_id": item["call_id"], "content": item["output"]}
                role = "user"
            else:
                for c in item.get("content", []):
                    messages.append({"role": item["role"], "content": [{"type": "text", "text": c.get("text", "")}]})
                continue
            if messages and messages[-1]["role"] == role:
                messages[-1]["content"].append(block)
            else:
                messages.append({"role": role, "content": [block]})
        if body.get("instructions", "").startswith("You summarize"):
            decision = ("text", "Earlier: the user asked for a setting change and a run.")
        else:
            decision = decide(messages)
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.end_headers()
        if decision[0] == "text":
            for w in decision[1].split(" "):
                self._event("response.output_text.delta", {"type": "response.output_text.delta", "output_index": 0, "delta": w + " "})
        else:
            _, name, args = decision
            call = "call_" + uuid.uuid4().hex[:12]
            self._event("response.output_item.added", {"type": "response.output_item.added", "output_index": 0,
                                                       "item": {"type": "function_call", "call_id": call, "name": name, "arguments": ""}})
            text = json.dumps(args)
            for i in range(0, len(text), 24):
                self._event("response.function_call_arguments.delta",
                            {"type": "response.function_call_arguments.delta", "output_index": 0, "delta": text[i:i + 24]})
            self._event("response.output_item.done", {"type": "response.output_item.done", "output_index": 0,
                                                      "item": {"type": "function_call", "call_id": call, "name": name}})
        self._event("response.completed", {"type": "response.completed",
                                           "response": {"usage": {"input_tokens": 100, "output_tokens": 20}}})

    def _event(self, name, data):
        self.wfile.write(f"event: {name}\ndata: {json.dumps(data)}\n\n".encode())
        self.wfile.flush()
        if Handler.slow:
            time.sleep(Handler.slow)

    def _json(self, obj):
        data = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--slow", type=float, default=0.0)
    ap.add_argument("--drop-once", action="store_true")
    a = ap.parse_args()
    Handler.slow = a.slow
    Handler.drop_once = a.drop_once
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"model fixture on http://127.0.0.1:{srv.server_address[1]}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
