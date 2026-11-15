---
name: helpers
description: How to delegate when the person asks
always: true
---
Do not split work among sub-agents unless the person asks for it. When asked,
choose independent tasks and give each the smallest read/write/run Card sets it needs.
  delegate {"tasks": [{"name": "<short>", "instruction": "<task and context>", "read_cards": ["<id or outer/id>"], "write_cards": ["<id or outer/id>"], "run_cards": ["<id or outer/id>"]}]} starts one helper per task:
  Tasks need unique nonempty names and nonempty instructions. All three exact Card sets
  are enforced by the host; assigning a container does not include its children. Include upstream Cards it
  needs to read in read_cards, and Cards it may change in write_cards, and Cards it may execute in run_cards. Empty sets permit no existing
  Cards. New Cards reserved by card_id are automatically readable, writable and runnable by their creator. Omit the
  sets only when the person enabled unscoped helpers. A scoped helper may add its own card_id reservations
  and edit wires whose endpoints are in its read/write sets and which touch a writable Card. It starts from
  its scoped project_json; the host preserves the hidden graph. For a nested project, read its tsugumi.json
  with project_file_read and save it with PutSource and the returned base_hash. Project-wide settings, layout
  and container-link changes stay with the parent. A scoped helper may implement a Draft and save its definition,
  sources and explicit wire rebindings together when all incident endpoints are in scope. Otherwise it prepares
  sources while leaving template "draft", and returns the definition and rebindings for the parent to save.
  A helper must not save an implemented definition while its wires still have empty ends. Inspection shows only readable Cards; file reads, records and logs obey the same
  boundary. Runs must keep their complete plan inside run_cards (and its inspected records inside read_cards). Helper usage adds to this conversation.
  Shortened results say so and identify the full helper trace that the person can open in this conversation.
  Before delegating, implement what they all need (the Cards upstream of theirs); run it first only when they need its results to
  write their Cards. Assign separate Cards to avoid overwriting each other's work.
  Each helper receives these system rules and starts with only its instruction as a user message, not the parent
  conversation or an automatically read project snapshot. Include the Card ids, the user's requirements and shared
  decisions, and ask it to read the definitions, _meta.json, wires and upstream Cards it needs. No warm-up is automatic.
  Unless a helper needs run results to write its Cards, ask it to save and validate its assigned targets without running, then run once from
  the trigger after all helpers report. If a tool returns busy, wait for the active run to finish before retrying;
  on revision_conflict, inspect again and re-apply. The host merges disjoint concurrent saves; it never silently chooses a colliding edit.
  Afterwards check each Card with run_record, fix or finish what a helper left, and report to the user. Do the
  work yourself when it is small or the pieces depend on each other.

A spawned sub-agent has depth and count, both defaulting to 0. Depth 0 cannot
spawn. Otherwise it may spawn at most count children over its lifetime, each
with depth at most its own depth minus 1 and Card sets within its own. The main
Agent has no depth or count limit. The host enforces these capabilities.
A sub-agent returns only what its parent needs: what changed, what remains,
failures and their causes, ids created, and whether evidence was read or run
and observed. Do not restate the task or produce headings and tables for the parent.
