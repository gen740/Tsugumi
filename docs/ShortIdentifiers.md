# Agent reference identifiers

The Agent uses the same identities as the host/core records. There is no UUID
alias column, translation table, migration or dual-format reader. References are
not authorization: all existing grant, scope, receipt and transport checks remain.

## Inventory and decisions

Lengths below count ASCII characters, not tokens. “Recorded” means occurrences
inside tool-result bodies in the 21 nonempty saved conversations described
below; a zero means that this corpus does not exercise that reference.

| Reference | Previous source and form | New source, scope and decision | Recorded occurrences |
| --- | --- | --- | ---: |
| Project | `Store::open`, UUID, 36 | Host-managed projects reserve a 6-character ID before core startup. Direct cores create a 6-character identity in their own project state. The core carries it; host registration owns cross-project identity checks. Existing project name accompanies inspections. | 51 |
| Run | `Scheduler::accept`, UUID, 36 | `History::next_run_id`, positive decimal project sequence. Acceptance, counter and receipt commit together. Never reused after deletion, restart or portable-history import. Start time, target and marks remain available. | 178 |
| Invocation | Scheduler's integer invocation number plus Card path; no separate UUID | Retain the natural identity: `12/lorenz#2`. The number is scoped to a run and Card. Batch activation/index and implementation index remain integers. | Card strings 892; path segments 822 |
| Card / definition / nested Card path | Definition/canvas IDs; readable names, 1–128 characters per segment. `Store::allocate_card_id` appended four lower-case random characters to a stem. | Retain stored names and paths. New allocation uses the lowercase stem itself, then `-2`, `-3`, etc., checking reservations, canvas/definition IDs and filesystem names. Case-folded reservations prevent case aliases. A Card name can differ from its stable ID. | 881 generic `id` fields, including definition and canvas IDs |
| Model tool call | Provider `call_…` (37 in these recordings), `toolu_…`; provider function-item IDs can be 53 characters | Provider receive boundary constructs calls with host-allocated decimal conversation counters. The same ID is used in provider-native tool-use/function-call blocks and results; function-item IDs are discarded. No original call ID survives in the saved service call. Counters survive compaction/restart. | 358 calls; 716 input/output call-ID appearances |
| External service tool call | Web adapter `randomUUID`, 36 | `CallIdRequest` reserves a number in the same host conversation counter before dispatch. Approval and connection retries reuse that reservation. Caller-supplied IDs remain opaque request keys; clients no longer generate random ones here. | 0 separate external-tool conversations |
| Conversation | UI and `ConversationStore::Create`, UUID, 36 | Host allocates a 6-character random ID, collision-checks reservations and existing known conversation databases. Mac/Web request creation through RPC. Named `guide` and `external-<project>` conversations keep their natural identities. | 19 helper references | All IDs / result bytes before → after |
| Helper | `(parent_call, task)` plus SHA-256 scope prefix (64 + separator), followed by tool-call ID | Parent call, task index and child call counter: `1.0.2`. The parent/child hierarchy is also used for receipts, rereads, scratch and host-built chat rows. Helper name accompanies its result. | 19 helper `call_id` references in results; 19 saved helper contexts |
| Core operation / receipt | `Tools::ExecuteTool`, UUID-shaped hash of conversation/call, 36; random UUID on retries | Compose conversation and call: `abcdef-12`, with `-rN` for rejected CAS attempts and `-approved` for a newly approved attempt. Core validates a bounded filename-safe opaque operation key. UI request keys that never enter model context need no aliases. | 62 |
| Comment / finding | Mac/Web and host stamping/copying, UUID, 36 | Decimal number within the Card's comments. Mac/Web editors reserve numbers in the shared host; Agent stamping uses the same reservations. Occupied document IDs seed reservations. Copying a Card numbers its own comments. Comment text and Card identify the subject. | 0 generated comment IDs |
| Note / group | UI `note-`/`group-` + 8 hex, 13/14 | `note-N` / `group-N`, reserved by the shared host for each project and checked against all editable layout scopes, because existing edit APIs locate these across scopes. Their text/name remains intact. | 0 generated note/group IDs |
| Artifact | Scheduler UUID, 36; remote artifact truncated job hash formatted as UUID | `run/Card/invocation/output`, e.g. `12/lorenz/1/image`. Both backends use the same construction. The output identifies the value; artifact filename/member gives the simple name. A URI query percent-encodes this relative path. | 0 retained-artifact references |
| Log | No independent ID: run UUID + Card + invocation + stream + byte offset | Retain composition, with the decimal run number. Example: `log:12/lorenz?invocation=2&stream=stdout&offset=0`. | Included in run/Card counts |
| Value / saved input | Run UUID + Card + invocation + side/port; saved-input directory also includes run/invocation | Retain composition, with decimal run number. Exact value bytes and integrity digests are retained. | Included in run/Card counts |
| Recorded build environment | Nix/local and remote importer, SHA-256 of derivation/manifest/system, 64 | First-use decimal environment number within a run. `History::put_environment` compares the actual derivation/manifest/system, reuses an equal recorded environment and allocates the next number otherwise. The digest ID and its generation are removed. | 42 |
| Git version / commit | Git object ID, 40 | Host presents the usual 12-hex abbreviation; core Git lookups and host comment reads resolve an unambiguous prefix directly. Full Git identities remain in Git, without an alias table. Tags/branches keep their meaningful names. | 0 commits in this corpus |
| Remote execution job | SHA-256 of project/run/Card/invocation, 64 | Internal supervisor/journal identity remains in the remote protocol. The host omits the redundant `execution_job_id` from model-facing tool metadata; the Agent's evidence is the invocation identity. | 0 |
| Chat presentation row | Host derives text/tool/helper row IDs from positions and calls | Keep composition; helper prefixes now use the natural parent/task identity. These are UI identities rather than model tool arguments. | Not model input |
| RPC request / event / log-chunk sequence | Existing integer transport/database counters | Keep the existing counters and scopes. | No long random reference |

The random alphabet is `23456789abcdefghjkmnpqrstuvwxyz`: 31 lower-case letters
and digits, six characters (887,503,681 possibilities). It has no `0/O`, `1/l/I`,
JSON/URL/shell escaping, separators or case variants. OS randomness uses rejection
sampling; a scoped existence predicate retries collisions. Host reservations
contain only `(scope, id)`, never a former UUID or an alias. Composed references
can contain `/`; these are structured addresses, not random filename words.

## Exclusions

These are not shortened or rewritten as evidence aliases:

| Excluded identity | Source and present form | Reason |
| --- | --- | --- |
| Host user/external tokens | `host/main.cc`, `crypto::random_token(32)`, 64 hex characters | Connection capabilities; not model evidence. |
| Approval request / grant | `host/access.cc`, `crypto::uuid4()`, 36 characters | Authorization workflow references; may occur in a denied tool result and must retain unpredictability. |
| Execution scope secret / capability | `host/execution_broker.cc`, 64 hex random secret/capability; keyed scope tag is 64 hex | Binds project authority across host/worker connections. |
| Core life / remote worker instance | `remote_job.cc`, `workerd/src/journal.cc`, 64 hex | Exact lifetime/peer binding used by execution supervision. |
| Host server instance | `host/main.cc`, UUID, 36 | Transport instance identity; absent from ordinary model results. |
| OAuth state / nonce / PKCE verifier | `host/settings.cc`, 64 hex each; attempt ID is UUID (36), host URN is `urn:uuid:` + UUID (45) | Authentication and replay protection; not model evidence. |
| Pagination cursor | `History::make_cursor`, base64 JSON + `.` + 32 hex MAC characters; variable length | Signed continuation, not an object to cite. |
| Integrity digest | `crypto::sha256_hex` and Nix hashing, usually 64 hex or algorithm-tagged base64 | Existing CAS/content verification requires exact bytes. |
| Provider opaque IDs/signatures | Provider-generated; variable length | Signed provider protocol state cannot be rewritten. |
| Internal client/view/subscription/turn IDs | Client UUID generation and `Service::view_open`; UUIDs (36), or existing counters/composition | Not sent in the model's tool arguments/results or evidence links. No display namespace is added. |


- Connection/user/external/broker capability tokens, approval request/grant IDs,
  execution authorization scope keys, worker/core-life identities and TLS peer
  identities. They select or bind authenticated authority and must retain their
  existing unpredictability and exact comparisons. OAuth state, nonce, PKCE
  verifier and authentication attempt/client IDs likewise stay outside this
  change.
- Signed pagination cursors retain their authenticated payload/MAC. They are
  opaque continuation capabilities, not names to cite.
- SHA-256 file/value/schema/source/NAR/archive/fingerprint digests stay exact.
  In particular `base_hash`, `expected_hash` and read-page hashes are used for
  CAS and integrity verification. Replacing these by six characters would
  weaken real existing checks. The measurement separates them from reference
  IDs. The corpus contains 374 `hash` fields and 50 `source_hash` fields: 27,136
  digest characters remain. Environment IDs are references and do change.
- Nix store paths, registered folder resource/scope keys and their attached
  mount names remain actual filesystem/content/scope identities. User paths,
  source code, log contents and arbitrary user JSON are not rewritten. A long
  identifier in user data is not a service-generated reference.
- Provider-owned signed thinking/search/reasoning metadata is opaque provider
  protocol data. It is preserved for round trips. Service tool calls use the
  counters above; their old provider IDs are discarded rather than mapped.
- Internal temporary files, server/client/view/subscription identities that are
  never sent to the model have no added display aliases.

## Measured identifier budget

Source directory:
`/private/tmp/claude-501/-private-var-piagent-work-tsugumi--claude-worktrees-claude-feature-handoff-43945f/5ff6419f-ebcf-497b-b9c6-6bc76cf9d7a2/scratchpad/tuning/`.

`tests/tools/identifier_budget.py <directory>` opens saved SQLite databases
read-only. It matches every `runs3/*/summary.txt` conversation ID against the
project DBs moved by `bin/reset.sh` to `trusted/.reset-archive` and
`trusted2/.reset-archive`, plus the surviving project DBs. All **17** `runs3`
summary IDs were recovered. `runs2` contributes five conversations (one empty
warm-up). The `T2v-copy.sqlite` snapshot continues `T2u`'s conversation and is
counted once, using the newest saved snapshot. The script emits each exact DB
path and conversation ID so the source is inspectable.

Each row includes the main model context plus its saved helper contexts. Older
messages in conversation/helper archives are included once in their owning
context. A result that a helper and its parent both actually received is counted
in both model contexts. Provider-native `providerContent` is not double-counted
from its duplicate database representation.

“Before” comes from actual saved result/argument/citation text. “After” is a
**counterfactual replay of reference allocation on those same messages**. It
preserves existing Card names and user content; it does not estimate additional
savings from changing future Card allocation. The replay indexes parent/helper
call scopes, builds the composed operation references, and shortens typed run,
project, conversation and environment references. Each ordinary call ID is
counted in its assistant call and result. It is not a new live-model trial,
a tokenizer measurement, a cache-hit measurement or a billing measurement.
Integrity digests and opaque provider metadata are excluded from reference-ID
columns. Percentages use the original result-byte count as denominator.

| Conversation | Contexts | Calls | Original result bytes | Result ID characters before → after | All ID characters before → after | All IDs / result bytes before → after |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| runs2/L1-drafts-luna | 1 | 11 | 51,616 | 1,562 → 881 | 3,135 → 1,456 | 6.07% → 2.82% |
| runs2/L2-drafts-luna | 1 | 5 | 12,140 | 258 → 228 | 863 → 473 | 7.11% → 3.90% |
| runs2/L3-drafts-luna | 1 | 7 | 31,860 | 1,156 → 531 | 2,139 → 765 | 6.71% → 2.40% |
| runs2/M1-drafts-terra | 1 | 12 | 51,539 | 1,562 → 881 | 3,239 → 1,350 | 6.28% → 2.62% |
| runs2/warm | 1 | 0 | 0 | 0 → 0 | 0 → 0 | — |
| runs3/T1-luna-dyn3 | 1 | 9 | 22,309 | 361 → 296 | 1,273 → 560 | 5.71% → 2.51% |
| runs3/T1r-luna-dyn3 | 1 | 7 | 20,938 | 336 → 271 | 986 → 417 | 4.71% → 1.99% |
| runs3/T1s-luna-dyn3 | 1 | 11 | 45,340 | 1,194 → 534 | 2,438 → 780 | 5.38% → 1.72% |
| runs3/T2-terra-dyn3 | 4 | 27 | 121,359 | 2,544 → 1,229 | 5,231 → 1,626 | 4.31% → 1.34% |
| runs3/T2r-terra-dyn3 | 4 | 30 | 136,501 | 2,446 → 1,269 | 5,249 → 1,667 | 3.85% → 1.22% |
| runs3/T2s-terra-dyn3 | 4 | 27 | 116,776 | 2,247 → 1,142 | 4,936 → 1,541 | 4.23% → 1.32% |
| runs3/T2t-terra-dyn3 | 4 | 27 | 121,822 | 2,520 → 1,205 | 5,167 → 1,597 | 4.24% → 1.31% |
| runs3/T2u-terra-dyn3 | 3 | 37 | 156,445 | 4,783 → 2,677 | 9,825 → 4,142 | 6.28% → 2.65% |
| runs3/T2w-terra-dyn3 | 4 | 24 | 131,522 | 2,462 → 1,323 | 4,755 → 1,680 | 3.62% → 1.28% |
| runs3/T3-terra-split4 | 3 | 70 | 163,304 | 4,363 → 2,686 | 10,825 → 3,809 | 6.63% → 2.33% |
| runs3/T4-luna-mnist2 | 1 | 7 | 59,196 | 540 → 475 | 1,058 → 489 | 1.79% → 0.83% |
| runs3/T5-luna-mnist | 1 | 10 | 66,132 | 510 → 445 | 1,250 → 467 | 1.89% → 0.71% |
| runs3/T6-luna-mnist | 1 | 6 | 109,550 | 1,342 → 1,212 | 1,786 → 1,224 | 1.63% → 1.12% |
| runs3/T7-luna-mnist | 1 | 9 | 95,590 | 1,017 → 887 | 1,683 → 905 | 1.76% → 0.95% |
| runs3/T8-luna-mnist-knn | 1 | 11 | 58,636 | 509 → 444 | 1,331 → 478 | 2.27% → 0.82% |
| runs3/T9-luna-mnist | 1 | 6 | 72,117 | 767 → 702 | 1,211 → 714 | 1.68% → 0.99% |
| runs3/X-T1-invalid-stale-store | 1 | 5 | 33,294 | 603 → 538 | 973 → 548 | 2.92% → 1.65% |
| **Total** | **41** | **358** | **1,677,986** | **33,082 → 19,856** | **69,353 → 26,688** | **4.13% → 1.59%** |

The complete count includes results, tool arguments, typed numeric
invocation/revision references, call/result pairing IDs and local evidence
citations in assistant text:

| Component | Before | Replay after |
| --- | ---: | ---: |
| Tool-result reference characters | 33,082 | 19,856 |
| Tool call/result pairing IDs | 26,492 | 868 |
| Tool-argument reference characters | 7,436 | 5,266 |
| Assistant citation reference characters | 2,343 | 698 |
| **All counted reference characters** | **69,353** | **26,688** |

The replay removes **42,665 identifier characters (61.5%)**. Result IDs alone
fall by **13,226 (40.0%)**, from **1.97% to 1.18%** of the 1,677,986 original
result bytes. Including call IDs, arguments and citations, the reference cost
is **4.13% → 1.59%** of that denominator. Across the 21 nonempty conversations,
the average is 3,302.5 → 1,270.9 characters; the median is 2,139 → 905. Failed
trials, including the explicitly labelled stale-store trial, are retained in
the table instead of being silently removed.

The replay preserves recorded wording and citation labels; future labels are
instructed to include the reference and simple name. Static schema field names
and illustrative prompt placeholders are not object references and are not
counted. Repeated context sends and cached prefixes can multiply the cost, but
this corpus does not establish the corresponding token/billing savings.

## Persistence, assumptions and verification

`kStateFormat` stays **4**, with the updated schema digest. Host `host.sqlite`
adds scoped short-ID reservations. Home/project `agent.sqlite` adds durable
conversation/scope call counters. Core local history changes from version 10 to
**11**, portable `.tsugumi/runs.sqlite` from version 8 to **9**, adding the run
high-water mark; stored run/artifact/environment identifiers and scratch paths
also change. `.tsugumi/project-id` now contains six characters; `_meta.json`
comment IDs and `layout.json` note/group IDs use the composed values above.
Agent scratch moves from hashed conversation/helper directory names to their
actual short/composed IDs. The root workflow document and Card ABI do not gain
an alias format. **Existing development projects and incompatible host state need
to be recreated/set aside. There is no migration.**

- Run counters assume the existing single project core/ordered history writer.
  Actual history deletion and project copying are public features, covered by
  existing RPC tests; losing a counter there would reuse real references. The
  counter is portable even when every run has been deleted. Independently
  executing divergent copies of one project are not a distributed allocator.
- Conversation/call reservations assume persisted conversations outlive a model
  continuation. Recorded helpers and existing restart/compaction tests establish
  this use. Durable allocation prevents automatic reuse after compaction/restart;
  it is not an authorization check or a promise about arbitrary caller IDs.
- Annotation allocation assumes independent Mac/Web clients can edit one project,
  an existing supported host/document-merge path. Local counters would let two
  unsaved additions share an ID and merge as one object. Persistent host
  reservations serialize automatic allocations across those clients and Agent
  stamping; current document IDs seed reservations. This prevents allocator
  collisions, not deliberate duplicate IDs in hand-edited documents. Copying to
  a newly reserved Card starts a separate comment namespace.
- Artifact composition assumes one retained value per declared output of an
  invocation, the existing artifact contract. It also distinguishes remote
  outputs which previously shared the job-derived artifact UUID. No speculative
  additional random namespace is introduced.
- Case safety is structural: generated random names contain one case only.
  Filesystem tests create distinct directories and files using generated IDs on
  the actual test filesystem. They do not claim to emulate every filesystem.
- Link parsers share numeric-run fixtures, malformed/overflow/old-UUID rejection,
  and a composed artifact query. Public RPC tests read the exact run/log/artifact
  records those links select. No app is launched for these checks.

Validation (macOS, staged Git copies; no app launch):

- `nix build --cores 4 --max-jobs 1 …#ci-core` succeeded. The fixture-only
  follow-up rebuilt only the test package, reusing the native runtime.
- `checks.aarch64-darwin.web` succeeded: Web bundle/Wasm, typecheck and behavior
  suite, including shared service goldens, evidence links, comments and layout.
- `tests/check-fast.sh --no-nix`: 75 static tests passed, including schema digest
  and generated-file checks. The wrapper then returned nonzero because this
  sandbox refuses its final `ps` cleanup scan.
- C++ identifier/Card checks: 8 passed (forced random and ordinal collisions,
  independent scopes, restart, deletion, portable empty history, case-safe
  directories, real Card allocation). The broader remote-result binary passed
  76 tests with four existing Seatbelt skips; one new invalid Card-directory
  fixture was corrected and its eight relevant tests passed on the rebuilt
  package.
- Swift app package compiled, and 9 selected evidence/finding, comment, note,
  group, undo and marquee tests passed. SwiftPM build-tool sandboxing alone was
  disabled because nested Seatbelt execution is denied; product sandboxing was
  not changed. `macOS/CoreTests` service adapter tests: 21 passed, including
  the shared flatc goldens for both new requests/replies and omitted Host vectors.
- Final public Agent RPC checks: 9 passed, including Unix/WebSocket annotation
  allocation across restart, call counters, helpers and the model-facing
  tool-result shape. Host save/contract/version/merge checks: 23 passed.
- Full Agent RPC run before the annotation addition: 187 passed, one existing
  probe skip, 13 failures caused by `sandbox-exec: sandbox_apply: Operation not
  permitted` in shell scenarios. No assertions or sandbox policies were weakened.
- Full non-environment core Python suite: 475 passed, 4 xfailed, 25 failed.
  Two scratch fixtures still used UUID directory names; they were corrected for
  the new canonical numbers, and all 13 scratch/short-reference tests passed on
  rerun. The remaining failures were one Seatbelt shell scenario and 22 display
  worker tests (`worker exited before sending its result`). Visualization failures
  were reported by the owner as a known limitation on this Mac; these runs do
  not independently prove their cause. Tests were not skipped or weakened.
- The isolated runner exports `TSUGUMI_TEST_NO_SECRET_STORE=1`; Web tests recorded
  no Keychain OS commands. Its final `ps` cleanup is denied even after successful
  test processes, so the wrapper exit status is distinguished from assertions.

Linux compilation, real-provider/cache/token measurements, a real VM and GUI
navigation were not run. The user prohibited launching the app; Swift/Web
parser tests and public RPC record reads cover the evidence-link selections.
