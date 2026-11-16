---
name: helpers
description: How to delegate when the person asks
always: true
---
Do not split work among sub-agents unless the person's own message asks for helpers or parallel work, even when its pieces are independent. When asked,
choose independent tasks and give each the smallest read/write/run Card sets it needs.
  delegate {"tasks": [{"name": "<short>", "instruction": "<task and context>", "read_cards": ["<id or outer/id>"], "write_cards": ["<id or outer/id>"], "run_cards": ["<id or outer/id>"]}]} starts one helper per task:
  Give tasks unique nonempty names and instructions, exact Card paths (containers exclude descendants).
  Empty sets permit no existing Cards; omit sets only when the person enabled unscoped helpers.
  New Cards from a helper's own card_id reservations belong to it for reading, writing and running.
  Inspection, files, records and logs obey read_cards; runs keep their entire plan inside run_cards.
  Wire edits need both endpoints in read/write scope and a writable endpoint. Start from scoped project_json;
  the host preserves hidden Cards. A helper implements a Draft only when all incident endpoints are in scope;
  otherwise leave template "draft" and return the definition and rebindings. Never save an implemented
  definition with empty wire ends. Read nested tsugumi.json with project_file_read and save with PutSource.
  Project-wide settings, layout and container-link changes stay with the parent.
  Before delegating, implement shared upstream Cards; run first only if helpers need their results to write.
  A helper sees neither this conversation nor a snapshot: include Card ids, requirements and shared decisions.
  Ask it to read what it needs, save and validate without running unless it needs run results, and answer
  in at most five lines. Small changes use project_edit against the current document. A stale whole-document project_save is refused: read the project again and re-apply.
  When all answer, run once from the trigger, check each Card with run_record and finish what remains.
  Shortened results identify the full helper trace in the conversation. Helper usage adds to the conversation.

A spawned sub-agent has depth and count, both defaulting to 0. Depth 0 cannot
spawn. Otherwise it may spawn at most count children over its lifetime, each
with depth at most its own depth minus 1 and Card sets within its own. The main
Agent has no depth or count limit. The host enforces these capabilities.
A sub-agent ends with at most five lines, only what its parent needs: what changed, what remains,
failures and their causes, ids created, and whether evidence was read or run
and observed. Do not restate the task or produce headings and tables for the parent.
