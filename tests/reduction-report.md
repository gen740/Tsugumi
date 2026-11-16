# Test reduction implementation, 2026-11-16

Changes are uncommitted on `claude/test-reduction`. The work list was the owner's
`report-test-reduction.md` audit dated 2026-11-16. This report distinguishes
source deletions, removed repeated executions, and verification limitations.

## Source reduction and ownership

All numbered D/N/S sets are removed: **25 definitions, 42 collected cases,
392 gross body/decorator lines** from the audit. The valid-request assertion
after invalid layout measurements now lives in the retained Python socket
smoke. The opaque core layout matrix is one combined raw-object round trip
(escaped key, Unicode, extension and `1e999`); all Host validation vectors stay.
The complete 340-vector Host golden remains. Web codecs use two rich scenes,
one per method; Swift keeps its representative handwritten wire coverage.

Additional consolidation removes the generated optional-vector pseudo-test,
the duplicate Swift arranging test, the timed 150-cancellation stress test and
the timed Nix-preparation demo cancellation test. Other rewrites preserve their
test definitions. Net result: **29 fewer definitions, 61 fewer collected cases**
(42 numbered + 15 opaque layout + 1 unknown-field expansion + 4 additional
definitions, minus one new deterministic two-slot concurrency case). The 338
removed Web codec iterations and shorter stress loops are not counted as cases.

Across the audit's test directories, including their helpers and headers,
source lines decrease **64,737 → 63,902: 835 net lines removed**. This includes
the new 25-line Swift observation helper. The two opt-in benchmarks add 20
lines outside that corpus. CI scripts, product code, fixtures and this report
are excluded from that test-source count.

The never-run scratch assertion survives in `CoreLifecycle`; the Python wired
Draft save/replay scenario remains. Client templates retain menu identity,
source insertion and one actual create/undo/redo smoke; Host tests retain the
complete template/artifact matrix. Agent save RPC retains one policy bridge,
durable receipts, restart and VersionComments. Frame/note placement checks moved
to the surviving HostLayout client test before deleting duplicate arranging.

Actions and `ci-local.sh` now use `tests/ci/run-suite.sh`. The explicit negative
gtest filter and `shared_rules`, `sdk_pure`, `package_smoke` pytest manifests
make execution ownership reviewable. Shared rules, Web behavior, adapters and
SDK libraries run fully on x86 Linux; other targets retain native transports,
filesystem/process/worker/sandbox and package smokes. The formerly unexecuted
worker-condition target runs on the shared-rule owner; definition evaluation
runs on every native target. C# sandbox, examples and real remote execution now
have matching local/CI command lists. Local Linux represents the canonical
full job even on an ARM Mac. Actions keeps `-n auto`; local verification uses
two workers without changing suite selection.

## Typed product additions

These classifications preserve existing outcomes and human messages; they do
not prevent the underlying failure. Their assumption is that tests/callers must
distinguish the existing rejection reason while wording changes. Evidence is
the audit's actual recovery/parser/installer scenarios, not hypothetical use.

| Addition | Existing problem and effect |
| --- | --- |
| `HistoryOpenCode::{NotRegular,ArchiveConflict}` / `HistoryOpenError` | History path/archive failures previously exposed only prose. Startup records now carry `history_not_regular` / `history_archive_conflict`; generic exceptions carry `project_open_failed`. Existing incompatible-history fields remain authoritative. |
| `worker_condition::Code` in `Diagnostic` | The rejected-expression matrix distinguished categories through exact sentences. Thirteen enum categories plus the existing byte position retain that distinction. |
| Host `EnvironmentFailureCode` | Process exit, readiness timeout, cancellation and launch failure previously shared a message-only state. The state now carries those four reasons, or `None`. |
| `StateFormatError.directory` | The existing typed refusal now retains its normalized directory as data. |
| Existing Web `CoreHostError` extended with `path`; startup codes | Web startup used plain `Error` for incompatible state. It now preserves `state_format`, `startup_exit`, `readiness_timeout` and the directory. |
| Swift/Web session `saveFailure` | Session adapters flattened already typed save failures. They now retain the original error alongside display text. No new core rejection code. |
| Installer/uninstaller exit reasons | Installer failures now distinguish checksum (65), unavailable platform/dependency/download (69), installation/release/build/version failure (70); invalid uninstall arguments use 64. Rollback/file assertions remain. |

Harnesses retain Host `Failure.code` and structured startup-error records.
Other wording rewrites use existing `FailureKind`, source/line/cell/port fields,
neutral version records, selected option IDs and represented menu identities.
Injected data/redaction sentinels, schema identifiers and format placeholders
remain checked. Field names were added to the resource controls so tests can
identify fields independently of translated labels.

## Instability decisions

| Test or family | Why unstable / weak | Action | Surviving behavioral evidence |
| --- | --- | --- | --- |
| `NativeTerminalTest.HostOwnsPtyInputResizeCloseAndStripsSecrets` | Input/resize could precede shell startup. | Deterministic shell READY record, then input/resize; wait for close event. | Real PTY ownership, resize, input, close and secret stripping. |
| `CanvasWindowTests.routingBodiesMoveWithoutStartingAWire` | 96 synthetic mouse drags in an off-screen window depended on native event routing. | Delete synthetic drags; exercise the actual production hit shape at each flow/zoom. | Body/port exclusion geometry; production gesture code remains. This no longer claims end-to-end native mouse delivery. |
| Web `saveFailure`, `unsavedMerge`, `staleApply` | Limited microtask/tick loops raced callback scheduling. | Explicit save-start and release promises. | Dirty state, refusal identity, retained position, stale handling and successful retry. |
| Swift `SyncTests` delayed save/card creation | Fixed settling delay / bounded yields. | Injected prepare-save gate and explicit allocation release. | Concurrent edits, editor commit, save and allocation behavior. |
| Swift `Sync`, `ChatResume`, `ReviewFix`, `CardFileEditing`, `Terminal`, `HostLayout` transitions | Real 50–300 ms graces or bounded tick polling. | Observation notifications with a missing-event watchdog. | Actual model/revision/run/terminal state assertions. |
| Swift ChatResume streaming/stop/detach | A `--slow` response could finish before the UI observed it under load. | Model fixture holds after a visible text chunk until an explicit release; before-first-response cancellation holds the model process. | Running UI, stop/new message, detach without cancel, reconnect and persisted cancellation. Four focused cases passed. |
| Swift `ChatGPTSetupTests` | 30/50 ms termination/cancel graces and elapsed timeout. | Observable host counters, cancellation notification and explicit deadline gate. | Host cancellation acknowledgement, single quit reply and late acknowledgement behavior. |
| Swift execution-history layout | Repeated fixed waits for preferences/layout. | Observe measured frames and controls. | Narrow layout, header and control geometry. |
| HTML artifact preview | 1.5 s and 700 ms settling delays. | Wait for WebKit navigation completion; assert prevented click/submit synchronously. | Real WebKit isolation plus positive control retained; execution fails in this sandbox, as recorded below. |
| `test_cancel_at_any_moment_of_a_run_keeps_tsugumid_well` | 150 offsets could exercise zero effective cancellations. | Delete. | Gate-based worker cancellation, ignored-cancellation timeout and deadline-reason tests. |
| `test_status_and_cancel_during_nix_preparation` | Real build could finish before the test observed preparing. | Delete timed duplicate. | `test_build_wait_can_be_interrupted` controls evaluator readiness with a gate. |
| Immediate rerun and Swift notification bursts | Repetition and settling polls obscured the actual boundary. | Two focused runs; consume `RunFinished` notifications explicitly. | Acceptance at the finish boundary and contiguous public notification sequence. |
| Public copy/shell-import cancellation | 512 MiB transfers might finish before a polling test could cancel them. | Existing opt-in fault injection now holds the first transfer chunk; small files suffice. | Real Agent stop/disconnect, core cancellation receipt, zero revision change, no target and staging cleanup. |
| `test_notebook_child_is_reaped_on_cancel_or_deadline` | A one-second Timer could cancel before the PID file existed. | Cancellation/deadline callbacks become active only after child readiness. | Both typed reasons, PID reaping and no committed result. |
| Matplotlib SDK cancellation/deadline | 0.5 s cancellation and a wall-time ceiling. | Cancel after actual frame events; library expiry uses a controllable clock. | Emission order, cancellation, required outputs and expiry. |
| Web `wireDemos` frame budget | Shared-machine scheduling determined a 25 ms pass/fail threshold. | Move 200-frame budget to `tests/bench/wire-frames.ts`, opt-in. | All numerical route/collision/lane/order/determinism fixtures. |
| Heartbeat cadence | Wall-time upper bound could fail under load. | Move upper budget to `tests/bench/heartbeat.py`; explicitly advance lease time in the C++ fixture. | Typed cadence policy and an actual later heartbeat; lease/release ownership. |
| Web `sessionRestore` | Fixed 350 ms wait. | Advance virtual timers. | Actual saved/restored selection and state. |
| Web thumbnails / FigurePreview | Idle, linger and timeout waits depended on real scheduling. | Virtual timers, explicit request completion. | Lifetime, cache eviction, timeout and late-reply isolation. |
| Web/Swift model menus | Callback settling loops. | Await callback counts/continuations. | Offered IDs, checks, enabled state and actual actions. |
| Plugin terminal / shell duplicate joins | Fixed wait before interrupt/join. | Wait for output/SSE events; explicit shell release file. | Real cancel, duplicate-operation join and terminal interrupt behavior. |
| Remote worker startup | Bind-to-zero, close, then reuse a vacant port. | Worker binds port zero itself and writes its actual bound port in private readiness data. | Native TLS/worker smoke remains; Linux execution is unavailable here. |
| VM crash and project isolation | Fixed one-second crash grace and a six-second worker that could finish before the other project stopped. | Wait for the old transport EOF; use a deliberately unending worker with a readiness log, verify it stays running, then cancel explicitly. | Native core EOF wait passed; real shared-VM execution unavailable. |
| VM concurrency inference | Wall-clock spans inferred concurrency on a shared machine. | Remove the repeated span matrix; add a gated two-slot variant to the existing core cap test. | Core two-slot/eight-slot enforcement; VM keeps real unselected-branch Nix evaluation. |
| Broad shell/language/worker deadlines | Shared-machine elapsed upper ceilings. | Remove speed ceilings; retain typed terminal outcomes and process cleanup. | Real OS process/deadline/sandbox behavior. |

The transfer gate reuses the existing `TSUGUMI_ENABLE_FAULTS` /
`TSUGUMI_FAULT` mechanism. Its assumption is exactly the observed test race:
the test must cancel during copying, not hope that a large transfer is slow.
It signals the first chunk and checks cancellation while held. This prevents
the test's scheduling race; it adds no normal-use restriction or protection.
The model stream gate is likewise fixture-only.

## CI job-minutes

The audit's historical successful run
[43771991875](https://github.com/gen740/Tsugumi/actions/runs/43771991875)
used **5,084 seconds = 84.73 summed job-minutes** across 29 jobs. It is an older
commit, not an integration baseline rerun. The redundant ARM Web job cost 38 s;
the ARM adapter behavior step cost 40 s. Its job/setup is retained for smoke.

| Before | After / limits |
| --- | --- |
| 84.73 measured historical job-minutes | **Not measured after this patch.** The auditable projection is `83.43 + S - U + A` minutes: subtract 38 s Web and 40 s repeated adapter behavior; `S` is retained adapter smoke, `U` unmeasured shared-matrix/source savings, `A` the newly executed previously omitted targets. |

The audit's 123–124 s upper saving assumes eliminating the whole second adapter
job before smoke. That setup is retained here, so that number is not claimed.
The original x86 SDK job was the critical path (780 s); deleting warm Web/adapter
repetition alone removes approximately zero seconds from that historical
critical path. The audit's source-cleanup estimate (3–10 s per core run with six
workers) is not a measurement of these two-worker runs. No 25-to-10-minute local
verification claim is supported.

## Verification and remaining limits

Nix builds used fresh staged-copy repositories, four cores and one build job.
No real checkout files were staged or committed. Generated protocol/Agent docs,
Swift resources and bindings were regenerated through their normal tools/builds.
No app was launched and no real credentials or personal app state were read.

| Verification | Result |
| --- | --- |
| Staged `ci-core`, then narrowed C++ targets | Built successfully, including the final transfer-gate runtime and narrowed targets. |
| Host settings/layout/native terminal executable | 198 passed. |
| Worker condition / definition evaluation | 137 / 8 passed. |
| Core C++ result/store scenarios | 71 passed; four existing Seatbelt-unavailable skips. |
| Agent run-wait / copy gate scenarios | 15 run-wait plus two Agent copy cancellation cases passed. Shell import failed before the gate: `sandbox-exec: sandbox_apply: Operation not permitted` (exit 71). |
| Python source transfers | All 16 passed, including both new first-chunk cancellation gates. |
| Deterministic concurrency cap | Two-/eight-slot parameter cases passed. |
| Native crash EOF / VM sources | Actual native core crash/EOF passed; VM suites collected successfully, but real VM execution was unavailable. |
| Targeted core Python | 168 passed; another changed-module pass had 63 passed plus a Nix-output-format failure, corrected and individually rerun: 1 passed. These overlap and are not summed. |
| SDK pure-library files | 43 passed; subsequent virtual-clock Matplotlib recheck: 9 passed (overlapping). |
| Static / `check-fast.sh --no-nix` | 69 passed in the final fast check. |
| Full Nix Web type/behavior check | Passed, exit 0. |
| Swift latest app test compilation | Passed, including HTML preview changes. |
| Focused Swift pure menu/canvas/sign-in/policy cases | 21 passed. |
| Focused Swift streamed chat gates | Four passed. |
| Model fixture stream gate | Direct flush/hold/release check passed. |
| HTML WebKit positive control and protected preview | Two failed: ordinary WebKit cannot load/reach its local listener; protected navigation also never finishes in this sandbox. No green security claim. |
| Wider Swift / public notification case | Actual worker exits before returning a result. Wider run also lacked generated help/player assets and one invocation remained hung. These failures were not weakened or skipped. |
| Additional shell/SDK execution pass | 45 passed, one read-only shell case failed with exit 71; `sandbox-exec: sandbox_apply: Operation not permitted` prevents that sandbox here. |
| Linux / bubblewrap / real remote port-zero runtime | Could not run: no Docker/OrbStack daemon. macOS remote reception explicitly requires Linux. |

Except the Nix Web check/builds, isolated test commands return nonzero in final
cleanup because the sandbox refuses `ps`. Passing rows above describe test
bodies, not an all-green wrapper invocation. One wider Swift invocation could
not be interrupted: signalling its own launcher was also refused. It is not
reported as cleaned up.

Deliberately retained: real process/deadline and child-reaping waits, readiness
and filesystem condition polling with watchdogs, the seeded malformed-frame
suite, the unique 4,096-worker budget guard, real SDK/ABI/runtime/sandbox tests,
the paced demo's minimum sample delay, and Host state-format filesystem races.
These do not impose shared-machine performance ceilings. Prompt prose cleanup
was explicitly separate in the audit; mixed schema/model fixtures were kept.
Some `.hostService` scope remains because session construction reaches shared
settings; removing it without isolating those dependencies would create races.

The broader timing review is not a proof of zero flakes. Real VM execution was
unavailable: its crash wait now observes EOF, its shared-folder reload waits for
a valid response with a watchdog instead of a fixed attempt count, and its
project-isolation worker has an explicit stop. Browser one-turn yields and
real filesystem/process/readiness watchdogs remain. Those waits must not be
advertised as performance measurements or as proof that every possible
scheduling/environment failure has been eliminated.
