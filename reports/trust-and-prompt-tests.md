# Trusted folders and prompt test audit

Changes are uncommitted. No application bundle was launched, personal support directory accessed, or credentials read.

## Trusted folders

The owner reports repeated trust questions in real development trials. This change assumes the person intends to trust every project under an explicitly chosen folder. Host settings now store canonical `agent.trustedFolders` in the existing shared `desktop` profile; the Web adapter uses that same profile even for standalone Web. The host resolves the project path and compares path components, so a sibling with the same string prefix and a link pointing outside the trusted root do not inherit trust. A link pointing into the root does. Previously registered paths are preserved when other entries change, and replacing a registered root with an outside link does not transfer trust to that target.

The host applies the setting to trust queries, Skill catalogs, automatic Skill delivery and the check before using trusted project instructions. Removing a root restores the individual project decision (or Unknown), without recording inherited trust as an individual grant. Both UIs refresh open projects when host trust settings change. A list editor carries its read revision so a stale list cannot silently restore a folder another screen removed. Both UIs manage the same list beside the project trust setting and in application General settings. An inherited project toggle is disabled until its trusted root is removed. The CLI has no existing settings command surface, so no command family was added. The existing two-choice trust question remains unchanged.

`TSUGUMI_UI_TRUSTED_FOLDER=/absolute/root macOS/tools/run-ui.sh ...` registers the root before the first script step and before querying project trust. A registration failure stops the script with a failing launcher status. Like the existing script `setting` command, this saves a host setting until explicitly removed. A caller can select a separate host with `TSUGUMI_HOST_STATE`; the test wrapper already isolates host state. No new host-state lifecycle is introduced. The input is consumed only when there is a readable UI script, never from project content. Run artifacts remain for inspection. `run-ui.sh` already used `open -g`; its actual arguments are now covered by a mocked-launch test. The documented installation launch also uses `open -g`. Startup no longer forces activation, so LaunchServices can honor `open -g` for an ordinary development launch too. Selecting an existing window is also kept in the background during scripts/smoke tests. Actual sheet/window foreground behavior needs an interactive check; this task prohibits launching the app.

No database table or index changed. `kStateFormat` remains **4**, and the static schema digest is unchanged:

`e48092b38d73e331b9fbf520e0e628e2f5d8bbe7d02506957a79d6846c72d8e5`

## Removed wording assertions

The owner is changing instructions based on real model trials. A literal phrase assertion makes such a measured edit fail regardless of behavior; removing it removes that obstruction, but does not establish that any replacement prose performs well. No production prompt wording was changed here.

| Test / fixture | What it pinned; disposition |
| --- | --- |
| `tests/prompt/test_card_prompt_contract.py::test_object_api_and_recovery_are_explicit` | Literal Python/object API examples, `lowerCamelCase`, recovery sentence, `make that call now`, demo-list condition, `wait_seconds: 300`, `do not poll`, absence of the old `inputs.input(...)` example. Deleted the wording-only file. |
| `tests/prompt/test_delegate_contract.py::test_capability_contract_replaces_the_parallelism_setting` | Description phrases for default 0/0, lifetime count, depth arithmetic, Card sets, unlimited main Agent, permission setting; absence of `helperParallelism` in prose/source. Removed text checks; kept argument/schema constraints. |
| `tests/prompt/test_delegate_contract.py::test_unnamed_helpers_skill_owns_the_split_policy` | The exact split prohibition, its location/count/absence elsewhere, capability/report-length phrases and old system-prompt phrases. Deleted; runtime Skill metadata/override/delivery tests remain. |
| `tests/prompt/test_delegate_contract.py::test_delegate_texts_do_not_promise_legacy_host_behaviors` | Absence of old runs/together/each and retry/read/write promises; presence of scope, stale merge, placement, reservation and save-note phrases. Deleted. |
| `tests/static/test_agent_contract.py::test_delegate_schema_matches_the_core_contract` and prompt schema comparison | Equality also froze property descriptions. Comparisons now ignore descriptions recursively; removed those strings from `tests/fixtures/agent-contract.json`. |
| `tests/static/test_agent_contract.py::test_draft_contract_has_portless_ends_and_atomic_explicit_rebinding` | Literal empty-end example and SetProject/incoming/outgoing/same/single wording; absence of “provisional ports”. Deleted. Draft demo graph checks remain. |
| `tests/static/test_agent_contract.py::test_file_output_instructions_do_not_require_a_card_permission` | Absence of `filesystem`, presence of file/directory annotations and `run.output_directory` in prompt/CardGuide prose. Deleted; executable CardGuide examples remain. |
| `tests/static/test_agent_contract.py::test_file_tools_move_bytes_without_reading_and_explain_bounded_views` | Copy prohibition, byte limits, head/tail, scratch, exact write set, atomic failure phrasing, old description cap. Removed wording assertions; retained copy argument schema. |
| `tests/static/test_agent_contract.py::test_measured_card_and_recovery_instructions_are_ported` | Object API/recovery literals, helper policy placement, Draft/comment request tool keywords and helper/delegate absence, absence of delegate in a demo layout. Deleted; request substitution and runtime contracts remain. |
| `tests/static/test_agent_contract.py::test_run_wait_instructions_are_shared_with_external_mcp` | “capped at 300”, fixture description suffix, wait/still_running/do-not-poll phrases and old MCP phrase absence. Removed; retained wait argument schema. No description assertion remains on `agent-tools.json`; that catalog is also input data for the optional model probe, not a prose golden. |
| `tests/core/agent/catalog_test.cc::AgentCatalog.DraftSaveDescriptionReachesTheModelAndPreservesEmptyEnds` | “Drafts have no ports” and “single possible port must be stated”. Removed; kept actual encoded save/empty port checks and renamed test. |
| `tests/core/agent/catalog_test.cc::AgentCatalog.RunToolsOfferTheSameHostWait` | “capped at 300” in tool descriptions. Removed; retained schema checks. |
| `tests/core/agent/rpc_test.cc::AgentFixture.ToolCatalogsKeepGuidanceExclusiveToTheGuideRole` | Absence of “Use guide_show” from the main system prompt. Removed; kept role catalogs/schema and denied-call checks. |
| `tests/core/agent/skills_rpc_test.cc::SkillsFixture.AutomaticSkillsReachTheModelOnlyFromTrustedProjects` | Built-in helper policy sentence in untrusted/trusted provider requests. Replaced with structural Skill marker and fixture sentinels; retained trust, explicit selection and override behavior. |
| `tests/core/agent/guide_rpc_test.cc::GuideScript.GuidanceContainsNoOutputNumbersOrRemovedResultClaims` | Numeric and Japanese/English phrase blacklist in guide tour text. Deleted; graph transitions, demo targets, actions and tour state remain. Also removed `$demo` substring requirements in farewell/no-connection text. |
| `macOS/Tests/TsugumiAppTests/AgentRequestsTests.swift::generationButtonsRequestSavedValidatedRunResults` | Root/child English phrases, save/project_validate/run_record keywords and delegate absence. Replaced with substitution checks derived from the source templates. |
| `web/tests/agentRequests.ts` | Equivalent child/root and tool-keyword/delegate assertions. Replaced with source-derived substitution checks. |
| `web/tests/hostRules.ts` | `tsugumi_open` keyword in initial MCP instructions. Removed; retained source/generated equality and 4096-character budget. |
| `tests/python/test_claude_code_plugin.py::test_tools_and_instructions` | `tsugumi_open` in initialize instructions and the exact approval-description sentence for cleanup tools. Removed; retained role catalog, schema/transport behavior and instruction budget. |
| `tests/python/test_claude_code_plugin.py::test_open_starts_the_host_and_runs_core_tools_seen_by_the_page` | Opened/project_inspect keywords in first-open rules and “Rules” keyword absence on a repeat. Removed/replaced with comparison to the current generated source; retained full first-open delivery and one-time delivery mechanism. |
| `tests/fixtures/model_fixture.py` | Guide routing depended on the first sentence of the system prompt. It now routes from the guide role's `guide_show` capability. Synthetic scenario/user-message sentinels remain fixture data. |

Kept generated-source synchronization, role catalogs and schemas, budgets, trust/override ordering, runtime policy enforcement and explicitly synthetic fixture markers. Added `tests/core/test_prompt_examples.py` to parse actual FlatBuffers/Nix examples and Python ASTs, including Report/Run member names against the SDK source. The helper Skill's JSON examples are still parsed against task argument keys, without requiring a particular example or prose sentence.

The requested Japanese Skill guideline could not be written: the sandbox marks `.agents/` read-only, and both filesystem editing and `apply_patch` were rejected. `patches/optimize-tsugumi-prompts.patch` contains the exact addition and passes `git apply --check`.

## Validation

- Staged `ci-core` Nix build: passed with `--cores 4 --max-jobs 1`. Production and C++ test executables built successfully. No application bundle was launched.
- `tsugumi_host_settings_tests`: 194/195 passed. The new canonical-registration/validation/removal test passed. The unchanged `HostComponents.DemoReferencesKeepTheConfiguredRootAndReadLimit` expects the old file-list result from `List()`; the current unchanged implementation returns a catalog from `ja/en/index.json` and exposes file listing as `ListFiles()`. Neither that test nor implementation was altered here.
- `tsugumi_agent_rpc_tests`: 187 passed, 13 failed, 1 existing helper probe skipped (201 cases). All new trusted-folder cases passed, including Unix/WebSocket parity, nested paths, registration through a link, links in/out, restart persistence, removal/individual trust, replaced-root links, and one-time automatic Skill delivery/revocation at the deterministic model boundary. The 13 failures are shell scenarios blocked by `sandbox-exec: sandbox_apply: Operation not permitted`; assertions were neither weakened nor skipped. The existing `AgentShellProbe.DirectProjectRequest` only runs when re-executed from its parent shell isolation test with `TSUGUMI_SHELL_PROBE_ENDPOINT`.
- `pytest -q -n 2 tests/core/test_prompt_examples.py`: 3 passed (FlatBuffers/Nix parsing and Python AST/SDK member checks).
- `unittest discover -s tests/prompt`: 9 passed, including capability-based guide fixture routing with arbitrary prompt text.
- `bash tests/check-fast.sh --no-nix`: 75 static checks passed. Protocol docs, Agent generation, English strings and the state digest are in sync. `git diff --check`, `bash -n macOS/tools/run-ui.sh` and `git apply --check patches/optimize-tsugumi-prompts.patch` passed.

Every native/Python suite above ran through the isolated runner. The runner itself returned failure after test completion because this sandbox refuses `ps`, which its service cleanup uses. Test-body outcomes above are reported separately from that cleanup failure. Shared temporary entries reported during concurrent work were not inspected or deleted.

- Staged Nix Web check: passed (Wasm generation, Web build, TypeScript typecheck, behavior tests and demo player). The current worktree's behavior suite also passed with the same packaged dependencies after strengthening the inherited-trust fixture. Both Web runs recorded zero Keychain OS commands. The pre-existing duplicate translation key warning has identical values and was not changed.
- CLI/MCP adapters: `pytest -q -n 2 tests/python/test_claude_code_plugin.py` passed all 14 cases against the staged Web distribution and current native host.
- Swift CoreTests: all 33 XCTest cases and 20 Swift Testing cases passed, including `testInheritedProjectTrustCarriesItsHostFolder`.
- Swift App: build passed. Both script trust tests and the source-derived request substitution test passed in the full run and in a separate three-test run with `--skip-build --filter 'scriptTrust|generationButtonsSubstituteScopeAndCardList'`. The full App run recorded three Sync test failures (“the worker exited before returning a result”) and two issues in the ordinary WKWebView control test (no request observed and no script marker). WebKit sandbox-extension refusals were logged. It then stopped producing output for over five minutes and was interrupted; the full App suite is **not verified green**. SwiftPM's build sandbox was disabled to permit manifest/plugin execution inside this sandbox; application and test assertions were not disabled. Builds used four jobs, private cache/config/security paths under `macOS/.build`, and the isolated test wrapper.

The three failed Sync cases were `arrangingAFrameWithoutAPlaceCanBeSaved`, `publicRPCNotificationsKeepTheirOrderDuringRunBursts`, and `nameChangesKeepIdentityAndDoNotGiveOtherCardsResults`. The WebKit case was `anOrdinaryWebViewRunsAndLoadsTheHostileDocument`. These test bodies were not altered. All passing wrapped commands still returned failure during the runner's denied `ps` cleanup, as noted above.

Linux and actual application-window/sheet behavior were not run here. The task prohibits launching Tsugumi.app or taking the foreground. Temporary staging repositories were removed; build/test outputs remain in the Nix cache and ignored worktree build folders.

Shell cases that failed in this environment:

- `AgentProjectFixture.StopDuringSuccessfulShellImportDiscardsTheSources`
- `AgentProjectFixture.HelperShellCannotReadParentOrSiblingScratchByAnyPath`
- `AgentProjectFixture.ShellEditsAreAgentSavesAndFailedCommandsDoNotApplySources`
- `AgentProjectFixture.CancelledShellAndConcurrentExternalEditsNeverOverwriteCardSources`
- `AgentProjectFixture.ShellAttachedMountsNeedReadAndRemainReadOnly`
- `AgentProjectFixture.HelperFileOperationsEnforceExactSourceAndDestinationCards`
- `AgentProjectFixture.ShellCommandsRequireTheirOwnExplicitGrant`
- `AgentProjectFixture.LargeShellOutputIsShapedOnceWithAUsableContinuation`
- `AgentProjectFixture.ShellHidesControlCredentialsAndCannotSendDirectRPC`
- `AgentProjectFixture.DiagnosticShellSupportsPythonAsyncioWhenAvailable`
- `NestedFileFixture.NestedScratchIsPrivateToEachHelperAtEveryDepth`
- `NestedDepths/NestedHelperFiles.CopyAndShellStayInsideEveryParentsExactSets/0, where GetParam() = 1`
- `NestedDepths/NestedHelperFiles.CopyAndShellStayInsideEveryParentsExactSets/1, where GetParam() = 2`
