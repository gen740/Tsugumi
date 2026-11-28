# Agent service

Hostd owns Agent conversations, model connections and tool dispatch. See [permissions and data](security.md#7-the-models-api-keys-agent) and the [workbench controls](../Usage.md#working-with-the-agent).

## Commands in the project environment

Helpers have separate scratch and command/output directories, including unscoped helpers. A parent can hand a file to a helper by saving it in a Card included in that helper's `read_cards`; scratch is not shared. Cancelling before the durable save intent discards staged changes; once the journal intent is durable, recovery completes the atomic save.

Use `project_copy` to transfer an explicit list of `{from,to}` files from an attached folder, another Card or the caller's private scratch into Card sources. The host moves bytes through the normal save; the model receives only paths, sizes and SHA-256 hashes. Attached sources must be folders the person attached, Card sources need read scope, and destinations need write scope (helpers' exact sets apply). Links and traversal are refused. Copy and shell saves stream file bytes through private durable staging files and the normal journal; file contents do not cross the RPC frame or require a whole-file buffer. Document JSON remains limited to 8 MiB and the source manifest to 10,000 entries. For larger data, keep the folder attached and let a Card read it with a named `grants.hostPaths` entry, after approval.

`attached_folder_read` and Agent `project_file_read` return bounded pages: default 256 KiB, maximum 1 MiB, with total size and continuation offsets. A 200 MB attached file reads only the requested page. The UI can still fetch every page. Model-facing pages may be smaller to preserve valid JSON and correct continuation offsets within the tool-result budget. Use `project_copy` to move large files.

`project_shell` needs no decision: it runs only where the project
environment isolates it (the shared VM; other environments do not offer it).
The command runs in a staged project-shaped working directory. It can read
the scoped Card files and edit writable Cards' sources. Attached folders
appear at `attached/<SHA-256(folder path)[0:16]>/` and private `scratch/`
persists for the conversation or helper. Root `tsugumi.json`, `layout.bin`
and each Card's `_card.json`, `README.md`, `_inputs.fbs` and
`_outputs.fbs` are read-only in the staging area. Use `project_edit` for
small changes to those files.

A successful, uncancelled command produces one validated save. Revision and
file hashes detect concurrent changes. The timeout defaults to 10 seconds
and is capped at 30. Each returned stream holds up to 256 KiB, keeping both
ends when truncated. The model receives both ends within a 60,000-byte JSON
budget. Total sizes and `full_output_path` identify complete
`.stdout` and `.stderr` files under `.tsugumi/agent-files/`.

See the [RPC contract](Protocol.md#projectshell) and
[approval checks](security.md#the-agents-permission-boundary).

## Runs that end after the turn

A model turn's `run` result that is still active registers that run with its conversation and the turn that started it. Hostd watches it with `run_status` waits (`hostd/agent/src/run_wakes.cc`). When it ends, and the conversation's latest turn finished without a later turn from the person, hostd starts one continuation turn through the ordinary turn machinery. Its user message is a host note: the model reads the run id, its state and the failed Cards with their diagnostics' first line, and the presentation shows a `note` row (`run_ended`) instead of the text. A run ending while a turn runs waits for that turn to end. A stopped, failed or interrupted turn leaves the next step to the person, and a result showing the run's end (for example a wait in the same turn) means the Agent already saw it. Each run brings a conversation back at most once.

Only runs the Agent started in that conversation count; external tool calls (MCP clients) and the guide do not. `ConversationRunWakeRequest` (human only) turns it off for one conversation. The choice is a conversation-scoped host setting and `ConversationResult.run_wake` reports it. Watches live for the service's lifetime: after a host restart, a run that ends brings nothing back.

## Edits during a run

A run executes the document it was accepted with (`RunStatus.rev`, `execution_rev`); saves made while it runs create new revisions that the next run uses. The core still reads one thing from the project folder after acceptance: each Card's definition directory (with its project's `tsugumi.lock`) when the run first evaluates that Card. A save that changes those files for a Card in the active run's plan, or a Card whose Nix exports it reads, is refused (`busy`, naming the run and the Card) until the run ends. README, comments and `_card.json` are taken from the run's document, so they can change. Resident workers changed by a save are retired when the run ends, so the run keeps its own.

## Choosing Claude's features by purpose

Tsugumi uses Claude's models and features by choosing the one that fits each job. Each row's "Where to check" is code and tests.

| Purpose | Feature used | Where to check |
| --- | --- | --- |
| Make, fix, run and check Cards | Tool use in the Messages API. The tools are generated from the RPC contract (`protocol/tsugumi.fbs`) for the explicit `requests` allow list in `hostd/agent/Shared/agent.json` | [Tools and the RPC](#tools-and-the-rpc), `hostd/agent/src/catalog.cc`, `tests/core/agent/` |
| A model sized to the job | Implementing Cards uses the model the user chooses (default `claude-sonnet-5-5`, with effort and, on models that offer it, Fast mode selectable). With an Anthropic API key, the guide's conversation and the summaries of the thinking of the bird on the Canvas use the small `claude-haiku-5-5` (other providers use a small model of their own or the Agent's model) | [Connecting to models](#connecting-to-models), `modelOptions` in `hostd/agent/Shared/agent.json` |
| Judging from the figures a run makes (multimodal) | PNG and JPEG bytes in a tool's result, such as an image read from a kept artifact, reach the model as images. Views are live and not recorded, so a figure the model should read is saved as an artifact. The same in the app, the Web UI and the Claude Code plugin | `hostd/agent/src/tool_images.cc`, `cli/src/mcp.cc` |
| Implementing independent Drafts at the same time (subagents) | `delegate` starts tasks with depth/count capabilities and exact Card sets, and returns their reports and errors | [Splitting the work](#splitting-the-work-delegate), `tests/core/agent/project_rpc_test.cc` |
| Guiding through a fixed procedure (Skills) | The built-in Skill `$demo` (`hostd/agent/Shared/demo-skill.md`) guides through a demo, and Skills the user adds are loaded too | [Skills and conversations](#skills-and-conversations), [the guide](#the-thrush-chat-and-guide) |
| Showing work in progress | Tool arguments are received by streaming (`eager_input_streaming`), and the Card's code is shown on the Canvas as it is written | [The Agent on the Canvas](#the-agent-on-the-canvas), `hostd/agent/src/provider.cc` |
| Making long work faster and cheaper | Prompt caching on the system prompt and the tool definitions | `hostd/agent/src/provider.cc` |
| When up-to-date information is needed | The web search server tool (on by default; the user can turn it off) | [Web search](#web-search) |
| Working with outside agents (MCP) | The Claude Code plugin hands Tsugumi's tools over as an MCP server, and the work appears in the Web UI. A save that widens a Card's permissions waits for a person's approval | [Using it from Claude Code](../claude-code.md), `test_claude_code_plugin.py` |
| Guiding without an API key | A scripted guide that uses no model (`hostd/agent/Shared/guide-script.json`). Sends nothing anywhere and costs nothing | [The thrush's guide](#the-thrush-chat-and-guide) |

Besides Anthropic's Messages API, OpenAI's API and the ChatGPT plan are supported too ([Connecting to models](#connecting-to-models)).

## Connecting to models

The host-side `tsugumi-hostd` owns provider endpoints, revisioned settings and OAuth credentials. The clients keep settings screens, presentation snapshots and browser opening. hostd stores API keys in its owner-only store on every platform; clients save and remove them through it. Host RPC supplies settings get/update, secret-input provider set/status and sign-in begin/action/status; ordinary model selections contain no endpoint or credential. The app and the Web UI share one settings and ChatGPT account profile per host. Hostd validates OAuth, handles the callback and owns refresh and revocation. See [API keys and sign-in](security.md#7-the-models-api-keys-agent).

- Supports the Anthropic Messages API and the OpenAI Responses API, responding by streaming. OpenAI can be used with an API key or with the user's ChatGPT plan (Sign in with ChatGPT, the procedure for open-source tools) ([Security](security.md#the-chatgpt-plan-sign-in-with-chatgpt)).
- The chat's model menu chooses effort and Fast mode. hostd fetches and persists the model list from the provider; the effort levels and Fast mode offered for a model are only those confirmed in the provider's documentation (`modelOptions` in `hostd/agent/Shared/agent.json`). If nothing is chosen, these fields are not sent and the model's defaults apply.

## Tools and the RPC

The Agent and outside agents (MCP) read the same tool catalog: the core tools by name, the rest through the `tools` families. A model reads a shaped view of each reply, while receipts and the clients keep the complete one (`hostd/agent/src/tool_views.cc`): `project_inspect` summarizes Cards, wires, files and the last run unless `detail` asks for the snapshot; a save reports what changed, its diagnostics and the changed Cards' ports; recorded values appear decoded from their FlatBuffers schema as JSON; a still-running `run` or `run_status` names the `run_status` call that waits again (up to 1800 s); and a failed run lists each failed Card with its failure and the end of its log. Detailed file formats are on demand in `demo_read reference/project-files.md` (`hostd/agent/Shared/project-files.md`).

See the [RPC contract](Protocol.md) and [Agent permission boundary](security.md#the-agents-permission-boundary).

## Web search

[Web search settings and data sent](security.md#web-search).

## Links to the evidence

[Follow evidence from chat](../RunHistory.md#follow-evidence-from-chat).

## Generating Cards from Drafts

[Generate from Drafts](../Usage.md#working-with-the-agent) after describing each step and connecting its wires.

## Generating Cards from comments

[Generate from comments](../Usage.md#working-with-the-agent) to apply a request to existing Cards; [Versions](../Versions.md#comments) records comment changes.

## Splitting the work (delegate)

Independent Drafts can be implemented by helper Agents.

In either UI’s Agent settings, choose whether to allow unscoped helpers. Unscoped helpers are off by default so each helper’s work has a boundary a person can follow. The parent supplies separate `read_cards`, `write_cards` and `run_cards` arrays of exact permanent Card paths (`outer/step` includes only that Card). Inspection, files, saves, runs, records and logs enforce those boundaries in hostd; a container does not grant its children. Cards wired upstream of an assigned Card, transitively and through nested projects' boundaries, are readable as well, never writable or runnable, so a helper can follow what goes into its Cards; downstream Cards stay out unless assigned. A helper’s new `card_id` reservation grants it all three access kinds. Helpers may add their own reserved Cards and edit wires whose endpoints are in their read/write sets and which touch a writable Card. Small changes use `project_edit`, with keyed selectors and old/new values or exact text replacement, against the current document. The host preserves untouched document bytes. The canvas derives positions for Cards without saved coordinates. `project_connect` and the `canvas` family's `make_subgraph` and `layout` apply graph operations directly; `project_copy` moves files without reading their bytes into the model. Whole-document `project_save` is for creating a project and for a large restructuring; a stale revision is refused with a request to read again, keeping the proposed input. The parent handles project-wide settings, layout edits, container-link changes and tools that cannot identify a Card. A helper may implement a Draft and rebind its wires in one save when all incident endpoints are assigned; otherwise the parent finishes that transaction.

Helper usage adds to the conversation’s usage. A shortened delegate result says so and identifies its full trace in the conversation’s existing helper records.

## The Agent on the Canvas

[Agent activity and Draw thoughts](../Usage.md#agent-activity).

## Skills and conversations

The conversation list reads conversation metadata for the selected project.
The Host Agent service supplies the chat presentation. `EventsRequest`
returns events after the client's sequence cursor and a presentation update
with changed row indices and the total row count. A cursor older than the
presentation baseline receives a reset with the complete row set.

Conversation reads and event observation never start a turn. The Agent service persists the operation in flight; interrupted conversations expose that operation and wait for an explicit **Resume** request or a new message. Tool receipt reconciliation remains the boundary against replaying an interrupted operation.

A Skill with front matter `scope: [read]`, `scope: [read, run]` or another inline list of `read`, `write`, `run` starts directly as a scoped helper. Choose it from a Card's **Run Skill** menu, or use `$name` in chat with selected Card references. One access prompt shows the Skill source and exact Card sets before the helper starts. No main Agent delegation is involved and this approval grants no project access. `always: true` cannot be combined with `scope`: automatic application has no selected Cards and cannot enforce a helper boundary. The Skill catalog reports this as an invalid declaration with its reason. A missing or invalid declaration cannot start a scoped helper; runtime instructions cannot add access. Run cancellation is limited to this helper's own runs, whose entire execution plan was checked against its run set.

The bundled `$review` reads permissions alongside their apparent uses, sources of nondeterminism, resident-worker state and the build/run boundary. `$rerun-compare` reads and reruns selected Cards under the same conditions, comparing output content using the existing run comparison rules; incomplete retained data remains unknown. They contain natural-language instructions, with Japanese and English text, rather than language-specific analysers. Findings cite `card:outer/step?file=src%2Fmain.py&lines=12-18`, which opens those source lines, and state whether they were only read or run and observed. Keep individual findings with **Keep finding as comment**; those comments carry `finding: true` and are excluded from generation requests. Nothing rechecks, resolves or deletes them automatically. Each helper's trace remains expandable in the conversation after reconnecting.

The host-side `tsugumi-hostd` discovers Skills, parses their metadata and reports unreadable or oversized files through `SkillsListRequest` in `protocol/host.fbs`. The app and the Web UI keep folder selection, completion and menus; they send selected Skill names. For names supplied by Tsugumi, one Skill wins: the trusted project’s `skills/` folder, then the person’s registered folders (the last matching source wins), then the built-in Skill. Other duplicate names retain source order. The service reads contents beneath the project or a folder the person attached. A Skill must be smaller than 512 KiB. Discovery does not grant access to use its contents in a model prompt.

`ModelsListRequest` reads or refreshes the service's persisted catalog for each connection. Both clients use its `name` display field. An empty refresh leaves the stored catalog intact; a failed refresh does not change it. Clients retain only a presentation snapshot for rendering an open menu.

[Skills and conversations](../Usage.md#skills-and-conversations).

## The thrush (chat and guide)

The host-side `tsugumi-hostd` embeds and interprets `hostd/agent/Shared/guide-script{,.en}.json`. `GuideGetRequest` reads a session in Japanese or English; `GuideAdvanceRequest` reports an interest, a message or choice, a demo opening, a run observation, or a completed client action. The reply contains the step, text, choices, semantic actions, highlight and panel request. Scripted guidance works without a model connection.

Scripted tours and the model-backed `$demo` tour give short guidance: run, open the result's location, optionally change a parameter, Apply and rerun. The person reads and judges the results. Tours do not require checklists or confirmations of saving and running, and do not infer output values or changes from a successful run. In a script, `finished` names the step after success; other terminal states lead to the tour's `failed` step with logs and a retry button. Only steps with `finished` react to run endings. A result step also offers a run with unchanged settings to explore reproducibility. Detailed interpretation in model-backed chat is an optional response to a question, grounded in actual run records and saved artifacts.

Progress belongs to the host (`desktop`), shared by the app's windows and every Web page. Hostd persists the first-start answer, demo-offer answer and tour; the live step and run observations last for the service lifetime. A resumed tour uses that live step, or the script's start step after a service restart. Stale choices and repeated or superseded run observations do not advance the guide twice.

The clients draw anchors, position and animate the bird, open panels and perform WKWebView or DOM operations. They perform the reply's actions with the user's controls and report observations to hostd. Pages share the guide's changes with each other on the browser's `BroadcastChannel`. Model-backed guide conversations use the Agent contract.

[Start a guided tour](../Usage.md#working-with-the-agent).

## Automatic Skills and project trust

The built-in `helpers` Skill applies without `$helpers`. It says to delegate only
when the person asks, how to scope independent work and what a sub-agent returns.
A same-name override inherits automatic application. Other Skills can declare
`always: true` in their front matter to apply without being named.

The first opening on a machine asks “Do you trust this project?” once. The host
remembers the canonical path and the project folder’s device and inode, shared
by Mac and Web. Replacing the folder at that path asks again; ordinary saves,
including atomic replacement of `tsugumi.json`, keep the answer. The project’s
General settings can change it. Projects created here and copies of
bundled demos are trusted automatically. An untrusted project still opens for
reading; its Skills cannot override built-ins or apply automatically, while a
Skill explicitly selected by `$name` remains available with normal Read access.
Trust concerns Skill instructions; it grants no Card read, write or run access.

Each spawned sub-agent receives `depth` and `count`, defaulting to 0/0. A leaf
cannot spawn. Otherwise it can spawn at most `count` children over its lifetime,
with each child’s depth strictly lower and its exact read/write/run Card sets
contained in the parent’s corresponding sets. The main Agent has no such cap.
The host records consumption across retries and checkpoints. Unscoped helpers
still require the separate permission setting. Nested tasks appear under their
parent in the host-authored conversation presentation.

## What waits for the person

Asking the Agent to work on a project, or connecting an external agent over
MCP, is the person's go-ahead: the Agent reads, changes and runs the project
and uses its shell without a prompt. A few calls cross what the Agent's own
tools could not do, and wait for one decision of the person each
([permission boundary](security.md#the-agents-permission-boundary)). Attached
folders are readable because the person attached them. A helper's assigned
Card scope still limits what it can touch.
