#!/usr/bin/env python3
"""Local model provider fixture (Anthropic Messages API shape, SSE streaming).

It replaces only the model connection. Every decision is derived from the
conversation it receives, so the Agent can disconnect, restart and resume.
Scripted task, triggered by a user message like:

    set <card>.<param> to <value> and run, then report <card>

Steps: project_inspect -> project_save (SetProject with the param changed)
-> run (from the root trigger) -> run_status until terminal -> run_record of
the reported card -> a final explanation.

A permission task ("give <card> network access"): project_inspect ->
project_save turning the Card's network on -> a final line saying whether it
was saved or declined (the client asks the user for the approval).

A team task ("delegate the drafts a b c"): delegate with one helper per
Draft -> a final line quoting the helpers' reports. Each helper (its system
prompt names it) does project_inspect -> project_save of its own Draft only
(from the revision it inspected: the others save meanwhile) -> run from the
trigger (others run too) -> run_status -> run_record of its Card -> report.

A second scripted task implements Drafts ("Implement these Draft Cards ...
- <path> (definition <id>)"): project_inspect -> one project_save with
PutDefinition/PutSource per Draft -> project_validate -> run -> report.

A task from comments ("The user left comments on these Cards ...", with
"- <path> (definition <id>):" and "  * <comment id>: <text>" lines):
project_inspect -> one project_save that changes each Card and marks its
comments resolved, adding an input Card for a comment saying "add a Card
<id>" and a comment of its own for one saying "leave a comment" -> run -> report.

The guide (its system prompt begins "You are Tsugumi, the guide bird"): a
question gets project_inspect -> an attempt at project_save (the guide only
reads: it is refused) -> guide_show pointing at the run button with a run
button -> a line saying how many Cards it read, whether the save was refused
and which tools it was offered. An [Event] note gets the run state it names.

Sign in with ChatGPT (the ChatGPT plan): the fixture is also the OAuth
issuer (http://127.0.0.1:<port>): /api/accounts/authorize redirects at once
to the loopback callback (registering oaiapp_fixture_<n> for
dynamic_agent_client), /api/accounts/oauth/token exchanges the code (PKCE
S256) and rotates refresh tokens (a reused one ends the session), the ID token
is RS256-signed with a test key published at /.well-known/jwks.json, and
/api/accounts/oauth/revoke revokes. /v1/responses with a plan access token
checks the plan's rules (store false, stream true, no max_output_tokens, tools
in a namespace whose calls name it) and answers "exceed the usage limit" with
subscription_sharing_usage_limit_exceeded. GET /fixture/chatgpt reports what
it saw (refreshes, revocations, rule violations).

Web search ("search the web for <query>"), when the request declares the tool
(web_search on the Messages API, {"type": "web_search"} on the Responses API):
the fixture answers as the provider's own search does, with made-up results,
and searches nothing. On the Messages API "... with a pause" stops with
pause_turn on a search that has not run, and "... alongside inspect" calls
project_inspect in the same reply (the search runs once its result is back).
Like the API it refuses a request that sends the blocks back wrongly: without
the tool, without encrypted_content or encrypted_index, or with a search left
without its result. Without the tool it says it cannot search.
GET /fixture/requests lists every model request it saw (path, tools, include,
whether its text names web_search, whether it asks for a summary, and the
effort, speed and output limit it asked for). Like the APIs it refuses an effort
or a speed they do not know.

An empty reply: the latest user text "refuse this" gets no content and the stop
reason refusal, and "think only" a reply with nothing but a thinking block (on
the Responses API, a refusal with no output text). Like the Messages API it
refuses a request with empty content on any message but a final assistant one.

Reading many times ("read <path> <n> times", optionally "in <r> rounds"): n
reference_read calls in one reply, r replies in a row, then a line saying how
many results came and how many of them were cut.

The context limit (--context-limit): like the APIs, a request over it is refused
and nothing of it is answered: a 400 on the Messages API, response.failed on
the Responses API. A request's size is the input tokens the fixture reports.

Options:
  --slow SECONDS   delay between streamed chunks (cancellation tests)
  --drop-once      drop the first response mid-stream (retry tests)
  --chatgpt-expires SECONDS   lifetime of the plan access tokens (refresh tests)
  --context-limit TOKENS      refuse requests over this size, as the APIs do
  --window-cut-once           end the first project_save or read reply at the
                              context window (stop_reason model_context_window_exceeded)
  --fail-summaries N          refuse the first N requests for a summary
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import re
import socketserver
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlencode, urlparse

TASK = re.compile(r"set (\S+?)\.(\S+) to (.+?) and run(?:, then report (\S+))?", re.I)
TERMINAL = {"succeeded", "failed", "cancelled"}
PERMISSION = re.compile(r"give (\S+) network access", re.I)
STALE_READ = "validate at revision 0"
SEARCH = re.compile(r"search the web for (.+?)(?: (with a pause|alongside inspect))?\s*$", re.I)
# "write a broken project_save 2 times": that many replies end as usual but with
# arguments that are cut (or, "as a list", not an object), as a model may write
# them when the API does not check them; then it says what it was told.
BROKEN = re.compile(r"write a broken (\S+) (\d+) times( as a list)?")
READ_MANY = re.compile(r"read (\S+) (\d+) times(?: in (\d+) rounds)?", re.I)


# "look at the picture <path>": reads it and says how the image came; with
# ", which is refused" a request holding an image gets a 400.
PICTURE = re.compile(r"look at the picture (\S+?)(, which is refused)?$")


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
                text = b["content"]
                if isinstance(text, list):  # with images: its text comes first
                    text = next(p["text"] for p in text if p.get("type") == "text")
                try:
                    content = json.loads(text)
                except (ValueError, TypeError):
                    content = {"error": text}
                out.append((names.get(b["tool_use_id"], "?"), content, b.get("is_error", False)))
    return out


def system_text(body):
    """The system prompt as text (a string, or text blocks with cache_control)."""
    s = body.get("system", "")
    return s if isinstance(s, str) else "".join(b.get("text", "") for b in s)


def unanswered_tool_uses(messages):
    """tool_use ids not answered by tool_result blocks at the start of the next
    message: the Messages API rejects such a request, and so does the fixture."""
    for i, m in enumerate(messages):
        if m["role"] != "assistant":
            continue
        ids = [b["id"] for b in blocks(m) if b.get("type") == "tool_use"]
        if not ids:
            continue
        lead = []
        if i + 1 < len(messages) and messages[i + 1]["role"] == "user":
            for b in blocks(messages[i + 1]):
                if b.get("type") != "tool_result":
                    break
                lead.append(b["tool_use_id"])
        missing = [x for x in ids if x not in lead]
        if missing:
            return missing
    return []


def empty_message(messages):
    """The index of a message the Messages API refuses for its empty content:
    only a final assistant message may have none."""
    for i, m in enumerate(messages):
        if not m.get("content") and not (i == len(messages) - 1 and m["role"] == "assistant"):
            return i
    return None


def asks_empty_reply(messages):
    """"refuse" or "think" when the latest message's text asks for an empty reply."""
    text = "".join(b.get("text", "") for b in blocks(messages[-1]) if b.get("type") == "text") if messages else ""
    return "refuse" if "refuse this" in text else "think" if "think only" in text else None


def search_task(messages):
    """The search asked for by the latest user text, with what follows it."""
    for i in range(len(messages) - 1, -1, -1):
        if messages[i]["role"] != "user":
            continue
        texts = [b["text"] for b in blocks(messages[i]) if b.get("type") == "text"]
        if not texts:
            continue
        m = SEARCH.search(texts[-1].split("\n\n[Context")[0])
        return (m, messages[i + 1:]) if m else (None, [])
    return None, []


def search_replay_errors(messages, declared):
    """What the Messages API refuses in the search blocks sent back."""
    used, answered = {}, set()
    for i, m in enumerate(messages):
        for b in blocks(m):
            kind = b.get("type")
            if kind == "server_tool_use":
                used[b["id"]] = i
            elif kind == "web_search_tool_result":
                if b.get("tool_use_id") not in used:
                    return f"web_search_tool_result {b.get('tool_use_id')} has no server_tool_use before it"
                answered.add(b["tool_use_id"])
                if not declared:
                    return f"`web_search` tool use found, but no `web_search` tool was provided"
                content = b.get("content")
                if isinstance(content, list) and any(not str(r.get("encrypted_content", "")).startswith("enc-") for r in content):
                    return "encrypted_content is missing or was changed"
            elif kind == "text":
                if any(not str(c.get("encrypted_index", "")).startswith("idx-") for c in b.get("citations") or []):
                    return "encrypted_index is missing or was changed"
    last = len(messages) - 1
    only_results = bool(messages) and messages[last]["role"] == "user" and all(b.get("type") == "tool_result" for b in blocks(messages[last]))
    for tool_id, i in used.items():
        if tool_id in answered:
            continue
        if not declared:
            return f"`web_search` tool use found, but no `web_search` tool was provided"
        # Still open: only the end of the conversation may wait for its result.
        if not (i == last or (i == last - 1 and only_results)):
            return f"`web_search` tool use with id `{tool_id}` was found without a corresponding `web_search_tool_result` block"
    return None


def search_results(query):
    return [{"type": "web_search_result", "url": f"https://example.com/{i}?q={uuid.uuid5(uuid.NAMESPACE_URL, query).hex[:6]}",
             "title": f"Result {i} on {query}", "encrypted_content": f"enc-{i}", "page_age": "May 1, 2026"} for i in (1, 2)]


def search_reply(query, kind, rest):
    """A reply's blocks and stop reason for a search task on the Messages API."""
    uses = [b["id"] for m in rest for b in blocks(m) if b.get("type") == "server_tool_use"]
    done = {b.get("tool_use_id") for m in rest for b in blocks(m) if b.get("type") == "web_search_tool_result"}
    if uses and done.issuperset(uses):
        return [{"type": "text", "text": "The search is done."}], "end_turn"
    results = search_results(query)
    answer = [{"type": "text", "text": f"Found on {query}: "},
              {"type": "text", "text": "the first result says so", "citations": [{
                  "type": "web_search_result_location", "url": results[0]["url"], "title": results[0]["title"],
                  "encrypted_index": "idx-1", "cited_text": "says so"}]},
              {"type": "text", "text": "."}]
    if uses:
        # The search that waited runs now: its result comes first in this reply.
        return [{"type": "web_search_tool_result", "tool_use_id": uses[-1], "content": results}] + answer, "end_turn"
    use = {"type": "server_tool_use", "id": "srvtoolu_" + uuid.uuid4().hex[:12], "name": "web_search", "input": {"query": query}}
    if kind and kind.lower().startswith("with"):
        return [{"type": "text", "text": "Looking it up."}, use], "pause_turn"
    if kind:
        return [use, {"type": "tool_use", "id": "toolu_" + uuid.uuid4().hex[:12], "name": "project_inspect", "input": {}}], "tool_use"
    return [{"type": "text", "text": "Looking it up."}, use,
            {"type": "web_search_tool_result", "tool_use_id": use["id"], "content": results}] + answer, "end_turn"


def cache_marked(body):
    """Whether the request asks for prompt caching (system, tools or messages)."""
    return "cache_control" in json.dumps({k: body.get(k) for k in ("system", "tools")})


def trigger_of(inspect_value):
    """The Card whose own definition is the trigger template."""
    for d in inspect_value.get("definitions", []):
        if d.get("card_json") and json.loads(d["card_json"]).get("template") == "trigger":
            return d["id"]
    return "start"


DELEGATE = re.compile(r"delegate the drafts ((?:[\w-]+ ?)+)", re.I)
HELPER = re.compile(r'You are the helper "([\w-]+)".*?The Cards you own: ([\w, -]+)\.', re.S)


def rev_of(res):
    """The revision in a result, also when it is the Web server's whole delta or was cut to fit the context."""
    return str(res["value"]["rev"]) if "rev" in res.get("value", {}) else re.search(r'"rev":(\d+)', str(res)).group(1)


def last_delegate_task(messages):
    for i in range(len(messages) - 1, -1, -1):
        if messages[i]["role"] == "user":
            for b in blocks(messages[i]):
                if b.get("type") == "text" and (t := DELEGATE.search(b["text"])):
                    return t.group(1).split(), i
    return None, -1


def decide_delegate(messages, cards, start):
    results = tool_results(messages[start + 1:])
    together = any("run them together" in b.get("text", "") for b in blocks(messages[start]))
    if not results:
        tasks = [{"name": c, "cards": [c], "instruction": f"Implement the Draft {c} and run it."} for c in cards]
        return ("tool", "delegate", {"runs": "together", "tasks": tasks} if together else {"tasks": tasks})
    name, res, is_error = results[-1]
    value = res.get("value", {}) if isinstance(res, dict) else {}
    # Together: the helpers have written; the one run is this Agent's.
    if together and not is_error:
        if name == "delegate":
            return ("tool", "project_inspect", {})
        if name == "project_inspect":
            return ("tool", "run", {"base_rev": rev_of(res), "card": {"segments": [trigger_of(value)]}})
        if name in ("run", "run_status"):
            status = value.get("run", value)
            if status.get("state") not in TERMINAL:
                return ("tool", "run_status", {"run_id": status.get("run_id")})
            return ("text", f"Ran once: {status.get('state')}.")
    text = res.get("error", json.dumps(res)) if isinstance(res, dict) else str(res)
    return ("text", ("Delegation failed: " if is_error else "The helpers reported. ") + text)


def decide_helper(messages, helper, cards):
    """A helper implements its own Draft from what the app read for it, leaves
    base_rev to the app, runs from the trigger and reports."""
    results = tool_results(messages)
    own = cards[0]
    if not results:
        brief = next(b["text"] for b in blocks(messages[0]) if b.get("type") == "text")
        found = re.search(rf"^## {re.escape(own)}\n_card\.json:\n(.*?)\n_meta\.json:\n(.*?)\nports: ", brief, re.S | re.M)
        if not found:
            return ("text", f"{helper}: the app did not give me {own}.")
        meta = json.loads(found.group(2))
        card, fbs, nix = implement(json.loads(found.group(1)), meta)
        return ("tool", "project_save", {"changes": [
            {"value_type": "PutDefinition", "value": {
                "id": own, "card_json": json.dumps(card), "meta_json": json.dumps(completed_intent(meta))}},
            {"value_type": "PutSource", "value": {"path": f"definitions/{own}/_outputs.fbs", "bytes": fbs}},
            {"value_type": "PutSource", "value": {"path": f"definitions/{own}/default.nix", "bytes": nix}}]})
    name, res, is_error = results[-1]
    value = res.get("value", {}) if isinstance(res, dict) else {}
    if name == "project_save":
        if is_error:
            return ("text", f"{helper}: saving failed: {value.get('kind')} {value.get('diagnostic')}")
        return ("tool", "project_inspect", {})
    if name == "project_inspect":
        # (told to run together, it tries anyway: the app must refuse)
        return ("tool", "run", {"card": {"segments": [trigger_of(value)]}})
    if name in ("run", "run_status"):
        if is_error:
            return ("text", f"{helper}: the run could not start: {value.get('kind')} {value.get('diagnostic')}")
        status = value.get("run", value)
        if status.get("state") not in TERMINAL:
            return ("tool", "run_status", {"run_id": status.get("run_id")})
        return ("tool", "run_record", {"run_id": status.get("run_id"), "card": {"segments": [own]}})
    if name == "run_record":
        ok = (value.get("aggregate") or {}).get("succeeded", 0) > 0
        return ("text", f"{helper}: {own} implemented, its run {'succeeded' if ok else 'did not succeed'}.")
    return ("text", f"{helper}: done.")


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


def completed_intent(meta):
    """_meta.json as the save implementing a Draft leaves it: all it had, and
    what was empty written from it."""
    meta = dict(meta)
    meta["description"] = meta.get("description") or "Implements the Draft"
    meta["context"] = meta.get("context") or f"From the Draft: {meta['description']}. {meta.get('content', '')}".strip()
    return meta


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
                "id": def_id, "card_json": json.dumps(card), "meta_json": json.dumps(completed_intent(meta))}})
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


COMMENTS = re.compile(r"The user left comments on these Cards", re.I)
COMMENT_CARD = re.compile(r"^- (\S+) \(definition (\S+)\):\n((?:  \* .*\n?)+)", re.M)
ADD_CARD = re.compile(r"add a Card ([\w-]+)", re.I)


def last_comment_task(messages):
    for i in range(len(messages) - 1, -1, -1):
        if messages[i]["role"] == "user":
            for b in blocks(messages[i]):
                if b.get("type") == "text" and COMMENTS.search(b["text"]):
                    return b["text"], i
    return None, -1


def decide_comments(messages, text, start):
    results = tool_results(messages[start + 1:])
    if not results:
        return ("tool", "project_inspect", {})
    name, res, is_error = results[-1]
    value = res.get("value", {})
    if name == "project_inspect":
        defs = {d["id"]: d for d in value.get("definitions", [])}
        doc = json.loads(value["project_json"])
        changes, added = [], []
        for _, def_id, lines in COMMENT_CARD.findall(text):
            d = defs[def_id]
            meta = json.loads(d["meta_json"])
            asked = {m.group(1): m.group(2) for m in re.finditer(r"^  \* (\S+): (.*)$", lines, re.M)}
            for c in meta["comments"]:
                if c["id"] in asked:
                    c["resolved"] = True
                    c["resolution"] = f"Did what it asks: {asked[c['id']]}"
                    added += ADD_CARD.findall(asked[c["id"]])
            # An Agent may write back: a comment of its own, for the user to read.
            if any("leave a comment" in t.lower() for t in asked.values()):
                meta["comments"].append({"text": "Note from the Agent: the axis has no unit."})
            card = json.loads(d["card_json"]) if d.get("card_json") else {}
            card["symbol"] = "checkmark"
            changes.append({"value_type": "PutDefinition", "value": {
                "id": def_id, "card_json": json.dumps(card), "meta_json": json.dumps(meta)}})
        for cid in added:
            changes.append({"value_type": "PutDefinition", "value": {"id": cid, "card_json": json.dumps(
                {"template": "input", "params": [{"name": "text", "type": "string", "data": "x"}]})}})
            changes.append({"value_type": "PutSource", "value": {
                "path": f"definitions/{cid}/_outputs.fbs",
                "bytes": "namespace added;\ntable Msg { text:string; }\ntable Outputs { msg:Msg (required); }\nroot_type Outputs;\n"}})
            doc["canvas"]["cards"].append({"id": cid})
        if added:
            changes.append({"value_type": "SetProject", "value": {"json": json.dumps(doc)}})
        return ("tool", "project_save", {"base_rev": str(value["rev"]), "changes": changes})
    if name == "project_save":
        if is_error:
            return ("text", f"Saving failed: {value.get('diagnostic')}. The comments stay open.")
        inspect = next(r for n, r, _ in results if n == "project_inspect")
        return ("tool", "run", {"base_rev": str(value["rev"]), "card": {"segments": [trigger_of(inspect["value"])]}})
    if name in ("run", "run_status"):
        if is_error:
            return ("text", f"Followed the comments; the run could not start: {value.get('diagnostic')}")
        status = value.get("run", value)
        if status.get("state") not in TERMINAL:
            time.sleep(0.2)
            return ("tool", "run_status", {"run_id": status.get("run_id")})
        return ("text", f"Followed the comments and ran the workflow: {status.get('state')}")
    return ("text", "Done.")


def last_permission_task(messages):
    for i in range(len(messages) - 1, -1, -1):
        if messages[i]["role"] == "user":
            for b in blocks(messages[i]):
                if b.get("type") == "text" and (t := PERMISSION.search(b["text"])):
                    return t.group(1), i
    return None, -1


def decide_permission(messages, card, start):
    results = tool_results(messages[start + 1:])
    if not results:
        return ("tool", "project_inspect", {})
    name, res, is_error = results[-1]
    value = res.get("value", {}) if isinstance(res, dict) else {}
    if name == "project_inspect":
        d = next(x for x in value["definitions"] if x["id"] == card)
        body = dict(json.loads(d["card_json"]), network=True)
        return ("tool", "project_save", {"base_rev": str(value["rev"]), "changes": [
            {"value_type": "PutDefinition", "value": {"id": card, "card_json": json.dumps(body), "keep_meta": True}}]})
    if name == "project_save":
        return ("text", "Network access was declined; nothing was saved." if is_error else f"Saved: {card} may reach the network.")
    return ("text", "Done.")


def last_broken_task(messages):
    for i in range(len(messages) - 1, -1, -1):
        for b in blocks(messages[i]) if messages[i]["role"] == "user" else []:
            if b.get("type") == "text":
                return BROKEN.search(b["text"]), i
    return None, -1


def decide_broken(messages, task, start):
    rest = messages[start + 1:]
    results = tool_results(rest)
    if len(results) < int(task.group(2)):
        text = "[1, 2]" if task.group(3) else '{"changes":[{"value_type":"PutSource","value":{"path":"definitions/a/m.py","bytes":"print(1'
        return ("broken", task.group(1), text)
    sent = [b["input"] for m in rest for b in blocks(m) if b.get("type") == "tool_use"]
    told = str([r[1] for r in results])
    return ("text", f"Errors: {[r[2] for r in results]}. Inputs sent back: {json.dumps(sent)}. "
                    f"Told to write it again: {'Make the call again' in told}. Shown my text: {'INVALID_JSON' in told}.")


def last_read_many(messages):
    for i in range(len(messages) - 1, -1, -1):
        if messages[i]["role"] == "user":
            for b in blocks(messages[i]):
                if b.get("type") == "text" and (t := READ_MANY.search(b["text"])):
                    return t, i
    return None, -1


def decide_read_many(messages, task, start):
    after = messages[start + 1:]
    rounds = sum(1 for m in after if m["role"] == "assistant" and any(b.get("type") == "tool_use" for b in blocks(m)))
    if rounds < int(task.group(3) or 1):
        return ("tools", [("reference_read", {"path": task.group(1)})] * int(task.group(2)))
    results = [b["content"] for m in after for b in blocks(m) if b.get("type") == "tool_result"]
    return ("text", f"Read {len(results)} results, {sum('[cut' in r for r in results)} cut.")


def last_stale_read(messages):
    for i in range(len(messages) - 1, -1, -1):
        if messages[i]["role"] == "user" and any(STALE_READ in b.get("text", "") for b in blocks(messages[i])):
            return i
    return -1


def decide_stale_read(messages, start):
    results = tool_results(messages[start + 1:])
    if not results:
        return ("tool", "project_validate", {"at_rev": "0"})
    _, res, is_error = results[-1]
    value = res.get("value", {}) if isinstance(res, dict) else {}
    return ("text", f"Refused: {value.get('kind')}." if is_error else f"Checked at revision {value.get('rev')}.")


GUIDE = "You are Tsugumi, the guide bird"
EVENT = re.compile(r"\[Event\] The run (\S+) ended: (\w+)")


def decide_guide(messages, tools):
    start = max(i for i, m in enumerate(messages) if m["role"] == "user" and any(b.get("type") == "text" for b in blocks(m)))
    text = "".join(b.get("text", "") for b in blocks(messages[start]) if b.get("type") == "text")
    if e := EVENT.search(text):
        return ("text", f"The run {e.group(1)} ended: {e.group(2)}.")
    results = tool_results(messages[start + 1:])
    if not results:
        return ("tool", "project_inspect", {})
    if len(results) == 1:
        return ("tool", "project_save", {"base_rev": "0", "changes": []})
    if len(results) == 2:
        return ("tool", "guide_show", {"highlight": "run", "actions": [{"kind": "run", "label": "Run it"}]})
    cards = len(results[0][1].get("value", {}).get("definitions", []))
    refused = results[1][2]
    return ("text", f"I read {cards} Cards; the save was {'refused' if refused else 'accepted'}; "
                    f"tools: {' '.join(sorted(tools))}. Press Run to see it.")


def last_picture_task(messages):
    for i in range(len(messages) - 1, -1, -1):
        for b in blocks(messages[i]) if messages[i]["role"] == "user" else []:
            if b.get("type") == "text" and (p := PICTURE.search(b["text"])):
                return p, i
    return None, -1


def result_images(messages):
    """The media types of the images in tool results."""
    return [p["source"]["media_type"] for m in messages for b in blocks(m) if b.get("type") == "tool_result"
            and isinstance(b["content"], list) for p in b["content"] if p.get("type") == "image"]


def decide_picture(messages, path, start):
    rest = messages[start + 1:]
    if not tool_results(rest):
        return ("tool", "project_file_read", {"path": path})
    words = json.dumps(tool_results(rest)[-1][1])
    return ("text", f"Saw images: {result_images(rest)}. Data in the text: {'iVBOR' in words}. "
                    f"Told it was refused: {'did not accept' in str(rest)}.")


def decide(messages, system="", tools=()):
    """Returns ("tool", name, input) or ("text", message)."""
    broken, broken_start = last_broken_task(messages)
    if broken:
        return decide_broken(messages, broken, broken_start)
    if system.startswith(GUIDE):
        return decide_guide(messages, tools)
    if h := HELPER.search(system):
        return decide_helper(messages, h.group(1), [c.strip() for c in h.group(2).split(",")])
    team, team_start = last_delegate_task(messages)
    if team is not None:
        return decide_delegate(messages, team, team_start)
    many, many_start = last_read_many(messages)
    # Only while it is the latest request (a note about a cut reply is none).
    asked = [i for i, m in enumerate(messages) if m["role"] == "user" and any(
        b.get("type") == "text" and not b["text"].startswith("Your last reply was cut") for b in blocks(m))]
    if many is not None and many_start == asked[-1]:
        return decide_read_many(messages, many, many_start)
    comment_text, comment_start = last_comment_task(messages)
    draft_text, draft_start = last_draft_task(messages)
    task, start = last_task(messages)
    picture, picture_start = last_picture_task(messages)
    if picture and picture_start > max(start, draft_start):
        return decide_picture(messages, picture.group(1), picture_start)
    if comment_text is not None and comment_start > max(start, draft_start):
        return decide_comments(messages, comment_text, comment_start)
    perm, perm_start = last_permission_task(messages)
    if perm is not None and perm_start > max(start, draft_start):
        return decide_permission(messages, perm, perm_start)
    if (stale := last_stale_read(messages)) > max(start, draft_start, perm_start):
        return decide_stale_read(messages, stale)
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
    if is_error or "error" in res:
        # A tool that failed ends the task with what it said.
        return ("text", f"{name} failed: {res.get('error', res)}")
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


# ---- Sign in with ChatGPT ------------------------------------------------------
# A test-only RSA key (made for this fixture; it signs nothing else).
RSA_N = 0xbf2cd1af0e800319488acaa78805211479ef2759f4bb882a92b2a14b47628624bb2a774c6eed402d5cb5e42d1150f7ff4d95c3b2bdd8d69a9a240f47ba4b5e59c85fdfdc3753c5cf8119241d4f7fe890b5bbfbaad1f035a6b0fa5c0725240c4824a4105f6a074e31dc0d869bed985c1ecb62c0d8ba4219cbe0d3767ed9c57e6d8faede7532a58a6a9b0175445bea5c2f542e3cc3215320ee917366a69c3d3995f5f694576673652b15cd5704ef03d4b9d31c01356cb4f0a4bd61fe2157cbc136a4127bd47b8757fd9466755f99ffe3b6cff5fab2a89b8ac6616db3654a27f97050a251517d2026440046d2cb5677960c65960e982f714f4d85b94bf872143149
RSA_E = 65537
RSA_D = 0x94574db93d521d956fd1aa6086ce1c160c6b83286d1b5b1455d2600fceab679576da03c9fe45004872a8fd0def62d99c73f262f0fe1914a4d0386cd6e67a7712c9c5832a18905b6ca12b05f7ec693ee792ecf3e25f96b9bac796ecf3ea60e75acf7bb2eca694569b9fb9ab1ab1294949e7dd8ff05bced0bf7f8bf66585cb96464be865aada37477d67129b0343c2237869b282ba06512662888e2c0551f02309eb101bdd53b3ebf22997556b8cfdf2aca02d96ee77c3456bff5822d225b3360b5c2a1d5e9dc2effa6773d5aa9a94388774aa244cddf13cf0c6efdc406baf6c8ad65b4a65ff7612e7cbfaf0fa9606a4c1d8c6b8f7ced50c7d3cc0f097c32a5d51
KID = "fixture-key"
SHA256_DIGEST_INFO = bytes.fromhex("3031300d060960864801650304020105000420")
PLAN_SCOPE = "chatgpt.tokens.use.direct"


def b64url(b: bytes) -> str:
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def int_bytes(i: int) -> bytes:
    return i.to_bytes((i.bit_length() + 7) // 8, "big")


def sign_jwt(claims: dict) -> str:
    """RS256 (PKCS#1 v1.5 with SHA-256), with the test key."""
    head = b64url(json.dumps({"alg": "RS256", "kid": KID, "typ": "JWT"}).encode())
    body = b64url(json.dumps(claims).encode())
    k = (RSA_N.bit_length() + 7) // 8
    t = SHA256_DIGEST_INFO + hashlib.sha256(f"{head}.{body}".encode()).digest()
    em = b"\x00\x01" + b"\xff" * (k - len(t) - 3) + b"\x00" + t
    sig = pow(int.from_bytes(em, "big"), RSA_D, RSA_N).to_bytes(k, "big")
    return f"{head}.{body}.{b64url(sig)}"


class ChatGPT:
    """The issuer's state: codes, current tokens per client, what it saw."""
    lock = threading.Lock()
    expires = 3600
    registered = 0
    codes: dict = {}
    access: dict = {}      # access token -> client id (only the newest per client)
    refresh: dict = {}     # refresh token -> client id (only the newest per client)
    used_refresh: set = set()
    stats = {"refreshes": 0, "revoked": 0, "violations": [], "plan_requests": 0, "authorize": [], "model_lists": 0}

    @classmethod
    def issue(cls, client_id: str) -> tuple[str, str]:
        for table in (cls.access, cls.refresh):
            for k in [k for k, v in table.items() if v == client_id]:
                if table is cls.refresh:
                    cls.used_refresh.add(k)
                del table[k]
        a, r = "chatgpt-access-" + uuid.uuid4().hex, "chatgpt-refresh-" + uuid.uuid4().hex
        cls.access[a] = client_id
        cls.refresh[r] = client_id
        return a, r


def plan_violations(body: dict) -> list[tuple[str, str]]:
    """What the ChatGPT plan would refuse in a Responses request (preview limitations)."""
    out = []
    if body.get("store") is not False:
        out.append(("store", "store must be false"))
    if body.get("stream") is not True:
        out.append(("stream", "stream must be true"))
    # "service-tier override" is among what errors and recovery says to remove.
    for f in ("max_output_tokens", "temperature", "top_p", "metadata", "previous_response_id", "user", "truncation", "service_tier"):
        if f in body:
            out.append((f, f"{f} is not supported"))
    for t in body.get("tools", []):
        if t.get("type") == "web_search":
            continue
        if t.get("type") != "namespace" or not t.get("tools"):
            out.append(("tools", "function tools must be grouped in a namespace"))
    names = {t.get("name") for t in body.get("tools", [])}
    for item in body.get("input", []):
        if item.get("role") == "system":
            out.append(("input", "system messages are rejected; use instructions"))
        if item.get("type") == "function_call" and item.get("namespace") not in names:
            out.append(("input", "a function call must name its namespace"))
    return out


class Handler(BaseHTTPRequestHandler):
    server_version = "TsugumiModelFixture/1"
    slow = 0.0
    drop_once = False
    dropped = False
    cut_once = False
    cut = False
    cut_reason = "max_tokens"
    cached = False
    context_limit = 0
    fail_summaries = 0
    lock = threading.Lock()
    requests: list = []

    def log_message(self, *args):
        pass

    def issuer(self):
        return f"http://127.0.0.1:{self.server.server_address[1]}"

    def do_GET(self):
        url = urlparse(self.path)
        if url.path.rstrip("/") == "/v1/models":
            if (self.headers.get("authorization") or "").startswith("Bearer chatgpt-"):
                if self._plan_client() is None:
                    return
                with ChatGPT.lock:
                    ChatGPT.stats["model_lists"] += 1
                self._json({"models": [{"slug": "fixture", "display_name": "Fixture model", "visibility": "list"},
                                       {"slug": "fixture-hidden", "display_name": "Hidden", "visibility": "hide"}]})
                return
            self._json({"data": [{"id": "fixture", "type": "model"}]})
        elif url.path == "/api/accounts/authorize":
            self._authorize(parse_qs(url.query))
        elif url.path == "/.well-known/jwks.json":
            self._json({"keys": [{"kty": "RSA", "kid": KID, "use": "sig", "alg": "RS256",
                                  "n": b64url(int_bytes(RSA_N)), "e": b64url(int_bytes(RSA_E))}]})
        elif url.path == "/fixture/requests":
            with Handler.lock:
                self._json(Handler.requests)
        elif url.path == "/fixture/chatgpt":
            with ChatGPT.lock:
                self._json(ChatGPT.stats)
        else:
            self.send_error(404)

    def _authorize(self, q):
        """The browser's part: signs in at once and goes back to the callback."""
        one = {k: v[0] for k, v in q.items()}
        need = ("client_id", "ext_agent_host_id", "response_type", "redirect_uri", "scope", "resource", "state", "nonce",
                "code_challenge_method", "code_challenge")
        missing = [k for k in need if not one.get(k)]
        redirect = one.get("redirect_uri", "")
        with ChatGPT.lock:
            ChatGPT.stats["authorize"].append({k: v for k, v in one.items() if k not in ("id_token_hint",)} |
                                              {"id_token_hint": "id_token_hint" in one})
        if missing or not re.fullmatch(r"http://127\.0\.0\.1:\d+/auth/callback", redirect) or one["code_challenge_method"] != "S256":
            self._error(400, f"bad authorization request: {missing or redirect}")
            return
        if not one["ext_agent_host_id"].startswith("urn:uuid:"):
            self._error(400, "ext_agent_host_id must be an opaque urn:uuid")
            return
        back = {"state": one["state"]}
        with ChatGPT.lock:
            client = one["client_id"]
            if client == "dynamic_agent_client":
                if one.get("agent_name_hint") != "Tsugumi":
                    self._error(400, "agent_name_hint is the app's name")
                    return
                ChatGPT.registered += 1
                client = f"oaiapp_fixture_{ChatGPT.registered}"
                back["client_id"] = client
            code = "code-" + uuid.uuid4().hex
            ChatGPT.codes[code] = {"client_id": client, "nonce": one["nonce"], "challenge": one["code_challenge"],
                                   "redirect_uri": redirect, "resource": one["resource"], "scope": one["scope"]}
        back["code"] = code
        back["scope"] = one["scope"]
        self.send_response(302)
        self.send_header("location", redirect + "?" + urlencode(back))
        self.send_header("content-length", "0")
        self.end_headers()

    def _form(self):
        return {k: v[0] for k, v in parse_qs(self.rfile.read(int(self.headers["content-length"])).decode()).items()}

    def _token(self):
        f = self._form()
        with ChatGPT.lock:
            if f.get("grant_type") == "authorization_code":
                c = ChatGPT.codes.pop(f.get("code", ""), None)
                verifier = f.get("code_verifier", "")
                if (c is None or c["client_id"] != f.get("client_id") or c["redirect_uri"] != f.get("redirect_uri")
                        or c["resource"] != f.get("resource")
                        or b64url(hashlib.sha256(verifier.encode()).digest()) != c["challenge"]):
                    self._oauth_error(400, "invalid_grant")
                    return
                access, refresh = ChatGPT.issue(c["client_id"])
                now = int(time.time())
                id_token = sign_jwt({"iss": self.issuer(), "aud": [c["client_id"]], "sub": "user-fixture",
                                     "email": "fixture@example.com", "nonce": c["nonce"], "iat": now, "exp": now + 3600})
                scope = c["scope"]
            elif f.get("grant_type") == "refresh_token":
                token = f.get("refresh_token", "")
                if token in ChatGPT.used_refresh:
                    # A reused refresh token ends the session (rotation).
                    ChatGPT.refresh = {k: v for k, v in ChatGPT.refresh.items() if v != f.get("client_id")}
                    self._oauth_error(400, "refresh_token_reused")
                    return
                if ChatGPT.refresh.get(token) != f.get("client_id") or "scope" in f or not f.get("resource"):
                    self._oauth_error(400, "invalid_grant")
                    return
                access, refresh = ChatGPT.issue(f["client_id"])
                ChatGPT.stats["refreshes"] += 1
                id_token, scope = None, None
            else:
                self._oauth_error(400, "unsupported_grant_type")
                return
        out = {"access_token": access, "refresh_token": refresh, "token_type": "Bearer", "expires_in": ChatGPT.expires,
               "earliest_refresh_at": int(time.time())}
        if id_token:
            out["id_token"] = id_token
        if scope:
            out["scope"] = " ".join(sorted(scope.split(" ")))
        self._json(out)

    def _revoke(self):
        f = self._form()
        with ChatGPT.lock:
            if f.get("token_type_hint") == "refresh_token" and ChatGPT.refresh.pop(f.get("token", ""), None) == f.get("client_id"):
                ChatGPT.access = {k: v for k, v in ChatGPT.access.items() if v != f.get("client_id")}
                ChatGPT.stats["revoked"] += 1
        self._json({})

    def _plan_client(self):
        """The client of a plan access token, or a 401 (an old or revoked token)."""
        token = (self.headers.get("authorization") or "").removeprefix("Bearer ")
        with ChatGPT.lock:
            client = ChatGPT.access.get(token)
        if client is None:
            data = json.dumps({"error": {"code": "subscription_sharing_invalid_user", "message": "token not accepted"}}).encode()
            self.send_response(401)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        return client

    def _oauth_error(self, status, code):
        data = json.dumps({"error": code, "error_description": code}).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if self.path == "/api/accounts/oauth/token":
            self._token()
            return
        if self.path == "/api/accounts/oauth/revoke":
            self._revoke()
            return
        if self.path == "/v1/responses":
            self._responses()
            return
        if self.path != "/v1/messages":
            self.send_error(404)
            return
        raw = self.rfile.read(int(self.headers["content-length"]))
        body = json.loads(raw)
        names = [t["name"] for t in body.get("tools", [])]
        self._note_request(names, raw)
        if bad := self._refused_option(body):
            self._error(400, bad)
            return
        missing = unanswered_tool_uses(body["messages"])
        if missing:
            self._error(400, f"tool_use ids were found without tool_result blocks immediately after: {', '.join(missing)}")
            return
        picture, _ = last_picture_task(body["messages"])
        if picture and picture.group(2) and result_images(body["messages"]):
            self._error(400, "Could not process image")
            return
        empty = empty_message(body["messages"])
        if empty is not None:
            self._error(400, f"messages.{empty}: all messages must have non-empty content except for the optional final assistant message")
            return
        if kind := asks_empty_reply(body["messages"]):
            self._empty_reply(body, kind)
            return
        declared = "web_search" in names
        if bad := search_replay_errors(body["messages"], declared):
            self._error(400, bad)
            return
        search, rest = search_task(body["messages"])
        if search and declared:
            self._search_reply(body, search.group(1), search.group(2), rest)
            return
        if system_text(body).startswith("You summarize"):
            with Handler.lock:
                refuse, Handler.fail_summaries = Handler.fail_summaries > 0, max(0, Handler.fail_summaries - 1)
            if refuse:
                self._error(400, "this summary is refused")
                return
            decision = ("text", "Earlier: the user asked for a setting change and a run.")
        elif search:
            decision = ("text", "I have no way to search the web here.")
        else:
            decision = decide(body["messages"], system_text(body), names)
        size = len(json.dumps(body["messages"])) // 4
        if Handler.context_limit and size > Handler.context_limit:
            self._error(400, f"prompt is too long: {size} tokens > {Handler.context_limit} maximum")
            return
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.end_headers()
        usage_in = len(json.dumps(body["messages"])) // 4
        usage = {"input_tokens": usage_in}
        if cache_marked(body):
            # The system prompt and tools are cached by the first request and read by the next ones.
            with Handler.lock:
                first, Handler.cached = not Handler.cached, True
            usage["cache_creation_input_tokens" if first else "cache_read_input_tokens"] = 400
        self._event("message_start", {"type": "message_start", "message": {"usage": usage}})
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
        elif decision[0] == "broken":
            _, name, text = decision
            self._event("content_block_start", {"type": "content_block_start", "index": 0,
                                                 "content_block": {"type": "tool_use", "id": "toolu_" + uuid.uuid4().hex[:12], "name": name, "input": {}}})
            for i in range(0, len(text), 24):
                self._event("content_block_delta", {"type": "content_block_delta", "index": 0,
                                                    "delta": {"type": "input_json_delta", "partial_json": text[i:i + 24]}})
            self._event("content_block_stop", {"type": "content_block_stop", "index": 0})
            stop = "tool_use"
        elif decision[0] == "tools":
            with Handler.lock:
                cut = Handler.cut_once and not Handler.cut
                Handler.cut = Handler.cut or cut
            if cut:
                # The reply ends at the limit before anything of it was written.
                self._event("message_delta", {"type": "message_delta", "delta": {"stop_reason": Handler.cut_reason}, "usage": {"output_tokens": 20}})
                self._event("message_stop", {"type": "message_stop"})
                return
            for i, (name, args) in enumerate(decision[1]):
                self._event("content_block_start", {"type": "content_block_start", "index": i,
                                                     "content_block": {"type": "tool_use", "id": "toolu_" + uuid.uuid4().hex[:12], "name": name, "input": {}}})
                self._event("content_block_delta", {"type": "content_block_delta", "index": i,
                                                    "delta": {"type": "input_json_delta", "partial_json": json.dumps(args)}})
                self._event("content_block_stop", {"type": "content_block_stop", "index": i})
            stop = "tool_use"
        else:
            _, name, args = decision
            with Handler.lock:
                cut = Handler.cut_once and not Handler.cut and name == "project_save"
                Handler.cut = Handler.cut or cut
            self._event("content_block_start", {"type": "content_block_start", "index": 0,
                                                 "content_block": {"type": "tool_use", "id": "toolu_" + uuid.uuid4().hex[:12], "name": name, "input": {}}})
            text = json.dumps(args)
            if cut:
                # The reply ends at the output limit inside the tool call.
                self._event("content_block_delta", {"type": "content_block_delta", "index": 0,
                                                    "delta": {"type": "input_json_delta", "partial_json": text[:15]}})
                self._event("message_delta", {"type": "message_delta", "delta": {"stop_reason": Handler.cut_reason}, "usage": {"output_tokens": 20}})
                self._event("message_stop", {"type": "message_stop"})
                return
            for i in range(0, len(text), 24):
                self._event("content_block_delta", {"type": "content_block_delta", "index": 0,
                                                    "delta": {"type": "input_json_delta", "partial_json": text[i:i + 24]}})
            self._event("content_block_stop", {"type": "content_block_stop", "index": 0})
            stop = "tool_use"
        self._event("message_delta", {"type": "message_delta", "delta": {"stop_reason": stop}, "usage": {"output_tokens": 20}})
        self._event("message_stop", {"type": "message_stop"})

    def _empty_reply(self, body, kind):
        """A reply with nothing to send back: declined before any output, or only thinking."""
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.end_headers()
        self._event("message_start", {"type": "message_start", "message": {"usage": {"input_tokens": len(json.dumps(body["messages"])) // 4}}})
        if kind == "think":
            self._event("content_block_start", {"type": "content_block_start", "index": 0, "content_block": {"type": "thinking", "thinking": ""}})
            self._event("content_block_delta", {"type": "content_block_delta", "index": 0, "delta": {"type": "thinking_delta", "thinking": "Hm."}})
            self._event("content_block_stop", {"type": "content_block_stop", "index": 0})
        self._event("message_delta", {"type": "message_delta", "delta": {"stop_reason": "refusal" if kind == "refuse" else "end_turn"}, "usage": {"output_tokens": 0}})
        self._event("message_stop", {"type": "message_stop"})

    def _note_request(self, tools, raw, include=None):
        body = json.loads(raw)
        with Handler.lock:
            Handler.requests.append({"path": self.path, "tools": tools, "include": include,
                                     "names_web_search": b"web_search" in raw, "summary": b"You summarize" in raw,
                                     "eager": b"eager_input_streaming" in raw,
                                     "effort": body.get("output_config", body.get("reasoning", {})).get("effort"),
                                     "speed": body.get("speed", body.get("service_tier")),
                                     "output_limit": body.get("max_tokens", body.get("max_output_tokens"))})

    def _refused_option(self, body):
        """An effort or a speed the APIs do not know (a 400 there), and what the
        Messages API refuses in the tools and the history."""
        for t in body.get("tools", []):
            if "type" in t and "eager_input_streaming" in t:
                return f"tools: eager_input_streaming is not allowed on {t['type']}"
        for m in body.get("messages", []):
            for b in blocks(m) if "role" in m else []:
                if b.get("type") == "tool_use" and not isinstance(b.get("input"), dict):
                    return "tool_use.input: Input should be a valid dictionary"
        effort = body.get("output_config", body.get("reasoning", {})).get("effort")
        if effort is not None and effort not in ("low", "medium", "high", "xhigh", "max"):
            return f"unknown effort {effort}"
        if body.get("speed", "fast") != "fast" or body.get("service_tier", "fast") != "fast":
            return "unknown speed"
        return None

    def _search_reply(self, body, query, kind, rest):
        """A reply with the provider's search, block by block as the API streams it."""
        reply, stop = search_reply(query, kind, rest)
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.end_headers()
        self._event("message_start", {"type": "message_start", "message": {"usage": {"input_tokens": len(json.dumps(body["messages"])) // 4}}})
        for i, b in enumerate(reply):
            if b["type"] == "web_search_tool_result":
                self._event("content_block_start", {"type": "content_block_start", "index": i, "content_block": b})
            elif b["type"] == "text":
                self._event("content_block_start", {"type": "content_block_start", "index": i, "content_block": {"type": "text", "text": ""}})
                self._event("content_block_delta", {"type": "content_block_delta", "index": i, "delta": {"type": "text_delta", "text": b["text"]}})
                for c in b.get("citations", []):
                    self._event("content_block_delta", {"type": "content_block_delta", "index": i, "delta": {"type": "citations_delta", "citation": c}})
            else:
                self._event("content_block_start", {"type": "content_block_start", "index": i, "content_block": {**b, "input": {}}})
                text = json.dumps(b["input"])
                for at in range(0, len(text), 8):
                    self._event("content_block_delta", {"type": "content_block_delta", "index": i, "delta": {"type": "input_json_delta", "partial_json": text[at:at + 8]}})
            self._event("content_block_stop", {"type": "content_block_stop", "index": i})
        self._event("message_delta", {"type": "message_delta", "delta": {"stop_reason": stop}, "usage": {"output_tokens": 20}})
        self._event("message_stop", {"type": "message_stop"})

    def _responses_search(self, query):
        """A reply with the Responses API's hosted search (made-up results)."""
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.end_headers()
        results = search_results(query)
        call = {"type": "web_search_call", "id": "ws_" + uuid.uuid4().hex[:12], "status": "in_progress"}
        self._event("response.output_item.added", {"type": "response.output_item.added", "output_index": 0, "item": call})
        self._event("response.web_search_call.searching", {"type": "response.web_search_call.searching", "output_index": 0, "item_id": call["id"]})
        self._event("response.output_item.done", {"type": "response.output_item.done", "output_index": 0, "item": {
            **call, "status": "completed",
            "action": {"type": "search", "query": query, "queries": [query], "sources": [{"type": "url", "url": r["url"]} for r in results]}}})
        text = f"Found on {query}."
        self._event("response.output_text.delta", {"type": "response.output_text.delta", "output_index": 1, "delta": text})
        self._event("response.output_item.done", {"type": "response.output_item.done", "output_index": 1, "item": {
            "type": "message", "role": "assistant", "content": [{"type": "output_text", "text": text, "annotations": [{
                "type": "url_citation", "url": results[0]["url"], "title": results[0]["title"], "start_index": 0, "end_index": len(text)}]}]}})
        self._event("response.completed", {"type": "response.completed", "response": {"usage": {"input_tokens": 100, "output_tokens": 20}}})

    def _responses(self):
        """The same decisions over the OpenAI Responses API wire format."""
        raw = self.rfile.read(int(self.headers["content-length"]))
        body = json.loads(raw)
        declared = any(t.get("type") == "web_search" for t in body.get("tools", []))
        self._note_request(["web_search"] if declared else [], raw, body.get("include"))
        plan = (self.headers.get("authorization") or "").startswith("Bearer chatgpt-")
        if plan:
            if self._plan_client() is None:
                return
            bad = plan_violations(body)
            with ChatGPT.lock:
                ChatGPT.stats["plan_requests"] += 1
                ChatGPT.stats["violations"] += [m for _, m in bad]
            if bad:
                data = json.dumps({"error": {"code": "subscription_sharing_unsupported_capability", "param": bad[0][0],
                                             "message": bad[0][1]}}).encode()
                self.send_response(400)
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
        elif self.headers.get("authorization") != "Bearer fixture-key":
            self.send_error(401)
            return
        if bad := self._refused_option(body):
            self._error(400, bad)
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
        if asks_empty_reply(messages):
            # A refusal is an output item of its own: no output text comes.
            self.send_response(200)
            self.send_header("content-type", "text/event-stream")
            self.end_headers()
            self._event("response.refusal.delta", {"type": "response.refusal.delta", "output_index": 0, "delta": "I can't help with that."})
            self._event("response.output_item.done", {"type": "response.output_item.done", "output_index": 0, "item": {
                "type": "message", "role": "assistant", "content": [{"type": "refusal", "refusal": "I can't help with that."}]}})
            self._event("response.completed", {"type": "response.completed", "response": {"usage": {"input_tokens": 100, "output_tokens": 0}}})
            return
        search, _ = search_task(messages)
        if search and declared:
            self._responses_search(search.group(1))
            return
        if body.get("instructions", "").startswith("You summarize"):
            decision = ("text", "Earlier: the user asked for a setting change and a run.")
        elif search:
            decision = ("text", "I have no way to search the web here.")
        else:
            decision = decide(messages)
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("x-request-id", "req_fixture")
        self.end_headers()
        size = len(json.dumps(messages)) // 4
        if Handler.context_limit and size > Handler.context_limit:
            # In the stream, as the API reports it.
            self._event("response.failed", {"type": "response.failed", "response": {"error": {
                "code": "context_length_exceeded", "message": "Your input exceeds the context window of this model."}}})
            return
        texts = [b.get("text", "") for m in messages if m["role"] == "user" for b in blocks(m) if b.get("type") == "text"]
        if plan and texts and "exceed the usage limit" in texts[-1]:
            # The plan's limit, after the stream began (as the API may send it).
            self._event("response.failed", {"type": "response.failed", "response": {"error": {
                "code": "subscription_sharing_usage_limit_exceeded", "message": "usage limit reached"}}})
            return
        if decision[0] == "text":
            for w in decision[1].split(" "):
                self._event("response.output_text.delta", {"type": "response.output_text.delta", "output_index": 0, "delta": w + " "})
        elif decision[0] == "tools":
            for i, (name, args) in enumerate(decision[1]):
                call = "call_" + uuid.uuid4().hex[:12]
                item = {"type": "function_call", "call_id": call, "name": name, "arguments": ""}
                if plan:
                    item["namespace"] = body["tools"][0]["name"]
                self._event("response.output_item.added", {"type": "response.output_item.added", "output_index": i, "item": item})
                self._event("response.function_call_arguments.delta",
                            {"type": "response.function_call_arguments.delta", "output_index": i, "delta": json.dumps(args)})
                self._event("response.output_item.done", {"type": "response.output_item.done", "output_index": i,
                                                          "item": {"type": "function_call", "call_id": call, "name": name}})
        else:
            _, name, args = decision
            call = "call_" + uuid.uuid4().hex[:12]
            item = {"type": "function_call", "call_id": call, "name": name, "arguments": ""}
            if plan:
                item["namespace"] = body["tools"][0]["name"]
            self._event("response.output_item.added", {"type": "response.output_item.added", "output_index": 0, "item": item})
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

    def _error(self, status, message):
        data = json.dumps({"type": "error", "error": {"type": "invalid_request_error", "message": message}}).encode()
        self.send_response(status)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _json(self, obj):
        data = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class LocalServer(ThreadingHTTPServer):
    """Without the reverse DNS lookup of HTTPServer.server_bind (socket.getfqdn),
    which comes before listening: the fixture only ever serves 127.0.0.1, and a
    CI runner can spend half a minute on that lookup in every test."""

    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--slow", type=float, default=0.0)
    ap.add_argument("--drop-once", action="store_true")
    ap.add_argument("--cut-once", action="store_true", help="cut the first project_save reply at the output limit")
    ap.add_argument("--window-cut-once", action="store_true", help="end the first project_save reply at the context window")
    ap.add_argument("--chatgpt-expires", type=int, default=3600, help="lifetime of the plan access tokens")
    ap.add_argument("--context-limit", type=int, default=0, help="refuse requests over this many tokens")
    ap.add_argument("--fail-summaries", type=int, default=0, help="refuse the first N requests for a summary")
    a = ap.parse_args()
    ChatGPT.expires = a.chatgpt_expires
    Handler.slow = a.slow
    Handler.drop_once = a.drop_once
    Handler.cut_once = a.cut_once or a.window_cut_once
    if a.window_cut_once:
        Handler.cut_reason = "model_context_window_exceeded"
    Handler.context_limit = a.context_limit
    Handler.fail_summaries = a.fail_summaries
    srv = LocalServer(("127.0.0.1", a.port), Handler)
    print(f"model fixture on http://127.0.0.1:{srv.server_address[1]}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
