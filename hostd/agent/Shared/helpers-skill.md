---
name: helpers
description: A helper you can use when a part of the work needs one: what it sees, returns and costs
always: true
---
You do the work yourself. If a part needs it, you can hand it to a helper: another Agent that delegate starts for
one task. It begins with only its instruction and the Cards in
its scope: it sees neither this conversation nor a project snapshot, and has its own context, so what it reads stays
there. All tasks of one delegate call run at the same time, and the call returns when the last one ends, with each
helper's final reply (at most 8 KiB; the full trace stays in the conversation). Its tokens count toward this
conversation, and it starts by reading again what it needs (the project, the reference, the files), so reading
that the parts share is paid once per helper: three helpers for three one-Card Drafts took as long as one Agent
writing them and about twice the tokens. What it costs you: writing the scope and an instruction with everything it needs (Card ids,
requirements, shared decisions, whether to run), waiting, and reading and checking the reports.
  delegate {"tasks": [{"name": "<short>", "instruction": "<task and context>", "read_cards": ["<id or outer/id>"], "write_cards": ["<id or outer/id>"], "run_cards": ["<id or outer/id>"]}]}
  Card sets are exact permanent paths (a container does not include its Cards); an empty set permits none, and sets
  are omitted only when the person enabled unscoped helpers. Cards a helper reserves with card_id are its own.
  Inspection, files, records and logs follow read_cards; a run's whole plan must be in run_cards. A wire edit needs
  both ends readable or writable and one end writable. A helper implements a Draft only when
  all incident endpoints are in scope, otherwise it leaves template "draft" and returns the definition and rebindings. A nested
  tsugumi.json is read with project_file_read and edited with project_edit. Project-wide settings, layout and
  container links stay with the parent. Guarded edits apply to current state; a stale whole-document project_save is refused.
  A helper with depth and count (both 0 by default) may start count helpers of its own, each with a smaller depth
  and Card sets within its own; depth 0 cannot start any. The main Agent has no such limit.

As a helper, your final reply is all your parent reads: what changed, the ids you created, what remains, failures
with their causes, and whether you ran or read the evidence, in a few lines.
