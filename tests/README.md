# Repository tests

Run commands from the repository root. Build instructions are in [Installing and building](../docs/INSTALL.md#building-from-source); [core/README.md](core/README.md) gives the core suite's detailed workflow.

Run `tests/check-fast.sh` before every push: the static tests (`tests/static`: tests that only read files of the repository, which CI runs first in its own job) and the Web UI Nix check, including the generated demo player (`--no-nix` skips it). Run the platform checks with `tests/ci-local.sh`. On macOS this runs the Mac checks locally and the Linux checks in Docker or OrbStack. Use `tests/ci-local.sh mac` or `tests/ci-local.sh linux` for one platform, and `tests/ci-local.sh agent` for the public Agent RPC fixtures.

Every test command must run through `build-ci/bin/tsugumi-test-env` (already used by CI and `ci-local.sh`). With a CMake build, use `python3 tests/run-isolated.py COMMAND ...`, for example `python3 tests/run-isolated.py ctest --test-dir build --output-on-failure` or `python3 tests/run-isolated.py /usr/bin/swift test --package-path ui/macos`. The runner creates short private temporary, support, cache, socket and disk scratch roots, stops services before removing them even on command failure or termination, and names the new entries left in `/tmp`, `/var/tmp` and the shared scratch root (not the per-user temporary folder of macOS, where system services create entries of their own): on a dedicated machine (`CI` set, or inside a Nix build) that fails the run; elsewhere it is a report, because entries made by other work on the same machine cannot be told apart. It only lists shared entry names; it neither traverses real project scratch nor deletes escaped data. Killing the runner itself with SIGKILL cannot execute cleanup.

### Secret-store isolation

Assumption: a developer runs tests on a signed-in Mac. This occurred in the Web
behavior suite: mocking RPC alone left the default provider adapter able to run
`security` against that person's login Keychain. Changing HOME cannot isolate
that store. `run-isolated.py` therefore forces `TSUGUMI_TEST_NO_SECRET_STORE=1`,
sets a private HOME (including Foundation's `CFFIXED_USER_HOME`), and removes
inherited Anthropic/OpenAI API keys. The Web OS runner and Mac Security runner
also unconditionally refuse when `TSUGUMI_TEST_RUN_ROOT` is present, even if the
switch is set to `0`. There is no real-store opt-in. Tests exercise credentials
by injecting a fake command runner, `KeychainStore` runner, or `MemorySecrets`.
This prevents accidental use of these production adapters; it is not an OS
sandbox for arbitrary malicious test code.

`ui/web/scripts/test.mjs` forces the switch before importing tests and creates a
private HOME/support root when run standalone. It also records and rejects any
`security` subprocess across the suite and requires the record to stay empty. Pytest's
`tests/conftest.py` forces it and requires the isolated wrapper. Both Swift test
executables link `Tests/SecretStoreIsolation`, whose startup constructor sets the
switch before Swift globals and requires the wrapper. Running `swift test`
without the wrapper consequently fails before tests start.

Audited paths (2026-11-16):

| Suite / entry | Route to a secret store before isolation |
| --- | --- |
| `ui/web/tests/thoughts.ts` | `chatRoutes` → `CoreHost.summarize/provider` → default `ProviderKeys.provision` → `keychainReader` → `security find-generic-password` (attributes and `-w`). RPC doubles did not replace this command runner. |
| `ui/web/tests/providerKeys.ts` | `source/provision/save/delete` exercise find, interactive add and delete commands, already with an explicit fake runner. `keychainReader` itself requires an injected runner. |
| Other Web adapter tests (`settings`, `modelMenu`, `folderAttachments`, `agentRealCheck`, `hostReadiness`) | Construct `CoreHost`/`chatRoutes`; current cases replace settings/models, use ChatGPT rather than API-key selection, or exercise startup only. The default adapter remains attached, so a new API-key path would reach the same commands. `ui/web/scripts/test.mjs` previously set no refusal switch. |
| `tests/python/test_web_server.py`, `test_claude_code_plugin.py`, `test_cli.py` | Start the actual bundled server (MCP can start it detached). Settings key-status/catalog, models, API-key conversations, summaries and version notes can call the same default adapter. Their shared `isolated_environment` now forces the switch and a private HOME as well. |
| `ui/macos/Tests` | App/session startup, chat/model availability (`ChatResumeTests`, `GuideTests`, `SyncTests`) and version-note requests can reach `CoreHostClient.provider/usable/models/versionNote` → `Keychain.load/loadWithoutAsking` → `SecItemCopyMatching`. Save/delete APIs also reach `SecItemUpdate/Add/Delete`; `exists` reads attributes. All now pass through `runSystemKeychain`. Without a support override, `CoreHostClient.stateDirectory` also selects `~/Library/Application Support/Tsugumi[-dev]/host-core/host.sqlite`; the mandatory wrapper supplies private support storage. |
| `ui/macos/CoreTests` | `AuthenticationTests` uses a local provider and explicit ephemeral fixture keys through Host RPC; it does not directly read Keychain. The package links the same adapter, so it needs the same startup protection. `SecretStoreTests` covers all Mac adapter operations with recording/refusing doubles. |
| Core host tests | `tests/core/host/components_test.cc` and Agent settings/auth tests use explicit temporary state roots: `host.sqlite` holds OAuth credentials and host secrets (`SqlSecrets`). `settings_store_test.cc` injects `MemorySecrets`. `harness/host.py` and C++ host fixtures always pass `--state-dir`; hostd requires it. No `SecItem*`, `security`, or import from `~/.codex/auth.json` / Claude credentials exists in these adapters. |

`ui/web/tests/secretStore.mjs` bundles the actual default `CoreHost` adapter with a
recording/refusing `child_process` replacement, exercises read/status/save/delete,
and asserts an empty OS-command record for both the standalone switch and the
wrapper marker with the switch disabled. `SecretStoreTests.swift` records the Mac
system boundary and fake adapter operations. Neither guard test needs real
credentials. The previously reported OS-command refusal setting was not present
in this checkout; the switch above is introduced here.


Hosts can select storage with `TSUGUMI_SOCKET_DIR` (a short existing directory for local sockets) and `TSUGUMI_RUN_SCRATCH_DIR` (an absolute disk-backed path without symlink parents). `tsugumi-hostd --socket-dir` selects its stable broker/environment socket directory, and `--usage-scratch-dir` selects the shared run scratch root. Direct project services accept `--scratch-dir` for evaluator work and `--run-scratch-dir` for invocation data. Run data in production is still retained until explicitly deleted; opening a never-run project does not create its run scratch folder.

The CI definitions are [ci.yml](../.github/workflows/ci.yml), [ci-platform.yml](../.github/workflows/ci-platform.yml) and the reusable [docs.yml](../.github/workflows/docs.yml). [tests/ci/select_jobs.py](ci/select_jobs.py) selects consumers from PR merge-base diffs or push before/after diffs. Static tests run alongside the platform workflow, and the final gate requires both.

Prose and `README.md` run the documentation build and tests. `docs/features.md` also runs prompt contracts, and `docs/CardGuide.md` runs its SDK examples and player consumers. Demo changes run documentation and the x86_64 Linux jobs that read bundled demos, including Guide RPC, version history, SDK, MCP and the Web player. Native core and worker changes run runtime consumers on both Linux architectures and macOS, including guest builds on ARM Linux. Host-only changes select Host RPC and its consumers. Swift changes run Mac tests. CLI and plugin changes run adapter tests, with Mac tests also selected for CLI changes.

`protocol/`, `flake.nix`, `flake.lock`, `nix/`, CMake, CI files and unclassified paths run everything, as do uncertain/empty diffs, nightly runs (03:17 JST) and manual dispatches. Local scripts use the same ownership partition (see Execution ownership and timing below). Branch protection requires only **CI**, the always-running gate: it requires static/selection success and every selected workflow/job to succeed, allowing only intentional skips. Remove individual platform and Documentation checks from required checks.

| Location | Coverage |
| --- | --- |
| [tests/core/](core/README.md) | Core behavior, public RPC and integration tests |
| `tests/python/` | SDKs, examples, demos, CLI/plugin/Web adapters and host/VM integration |
| `tests/static/` | Repository contracts, packaging, release and generated-file consistency |
| `tests/remote/` | C++ remote worker admission, scoping, transfers, same-core reconnects and core-lifetime expiry on Linux |
| `ui/macos/CoreTests/`, `ui/macos/Tests/` | Agent, RPC and app behavior |
| `ui/web/` | Browser UI type checks and behavior contracts |
| `tests/prompt/` | Offline prompt probe scoring |

The build generates C++/Python/Swift/TypeScript bindings and BFBS with `flatc`; these stay in build outputs and are not committed.

Web gesture, source editing, template and undo fixtures call the production host operation executor through `tsugumi_document_edit_driver`, with a controllable in-memory core transport. The Nix Web check builds only that driver against the installed production host archives. For local `npm --prefix ui/web test`, first build the CMake target `tsugumi_document_edit_driver` and set `TSUGUMI_DOCUMENT_EDIT_DRIVER` to its absolute path (normally `build/tests/core/tsugumi_document_edit_driver`). No daemon or sockets are needed for these tests.

CI runs deterministic core RPC and Agent tests on Linux and macOS. SDK and Web checks run on Linux; Swift and Seatbelt checks run on macOS. Real remote execution using `tsugumi-workerd` runs on x86_64 Linux through `tests/ci-remote-worker.sh`. The socket-free thought scheduler test is part of `tsugumi_host_settings_tests` in the Agent RPC job.

The large demo runs are opt-in; CI collects them but leaves `TSUGUMI_TEST_DEMO_RUNS` unset. Run them explicitly in the development environment:

```sh
TSUGUMI_TEST_DEMO_RUNS=1 nix develop -c python3 tests/run-isolated.py python3 -m pytest -q tests/python/test_demo_*.py
```

The shared-VM suites also require an explicit opt-in on an Apple silicon Mac with the VM image installed. Build the signed controller first, then run:

```sh
vm/macos/tools/build-vmctl.sh
TSUGUMI_TEST_CONTAINER=1 nix develop -c python3 tests/run-isolated.py python3 -m pytest -q \
  tests/python/test_shared_vm.py tests/python/test_cards_container.py tests/python/test_workers.py
```

The VM support directory and its store are disposable and removed at session end. `TSUGUMI_VM_X86=qemu` selects QEMU instead of Rosetta for x86_64 Linux Cards. These VM suites do not run in CI.

`tests/core/agent/environment_rpc_test.cc` exercises Host environment requests against hostd and an external fake `tsugumid` with authenticated project RPC. It covers readiness/registration, concurrent starts and aliases, process death and retry, resident-process adoption, explicit history recovery, folder withdrawal, stop and broker capability revocation. `environment_adapter_test.cc` supplies a narrow helper double to the same lifecycle service. The RPC scenarios use Unix sockets; the helper-double scenarios run without sockets. Neither launches a VM or invokes Nix. Build `tsugumi_agent_rpc_tests` and `tsugumi_host_settings_tests`; run them with `--gtest_filter='EnvironmentFixture.*'` and `--gtest_filter='EnvironmentAdapterTest.*'`, respectively. The CLI start/origins/failure scenarios in `tests/python/test_cli.py` run the Web host through hostd against `vm_helper_double.py`, a private FlatBuffers adapter with a native project daemon. `test_web_server.py` verifies environment residency and stop/restart through the same Host requests. `ui/web/tests/environmentHost.ts` and Swift `ServiceWireTests` check request encoding, events and omitted vectors without sockets. Mount selection and folder withdrawal are tested with the C++ helper double; Swift keeps no second mount-selection implementation.

`tests/core/agent/guide_rpc_test.cc` checks both embedded guide languages, script completeness and demo targets, interests and choices, fallback replies, failure/retry/change/farewell without confirmations, unchanged reruns, late and duplicate run observations, two windows sharing a guide, progress scope and restart/resume. Text is evaluated with real models rather than phrase assertions. Build `tsugumi_agent_rpc_tests`; run `--gtest_filter='GuideScript.*:GuideFixture.*'`. `GuideScript.*` needs neither sockets nor a model; `GuideFixture.*` needs sockets but no model or Nix daemon. Native bird geometry stays in `ui/macos/Tests/TsugumiAppTests/GuideTests.swift`; Web and Swift wire tests cover omitted guide vectors and presentation adapters.

Version notes have socket-free tree and localization tests in `tests/core/agent/version_text_test.cc` (`tsugumi_host_settings_tests --gtest_filter='VersionNote.*:VersionText.*'`) and settings validation in `tsugumi_host_settings_tests --gtest_filter='HostSettingsStore.VersionNotes*'`. `tests/core/agent/version_note_rpc_test.cc` exercises the human-only Host note request, core model selection, concurrent windows, persistence across restart, mode switching, failure/retry and empty diffs (`tsugumi_agent_rpc_tests --gtest_filter='AgentProjectFixture.VersionNote*'`). Those RPC scenarios need Unix sockets and a local model fixture. `ui/web/tests/versions.ts` checks the presentation request, settings, retries and late responses without a browser.

For documentation changes, run `nix run .#build-doc` and `nix run .#test-doc`. These commands use the shared generated demo player (`nix build .#site-player`); Web checks also test its generated data. Without Nix, site and Web builds and Web tests generate the player from `ui/web/node_modules` and `ui/web/shared/gen`.

## Manual tools

- `python3 hostd/tools/test-binary-cache.py`: check signed substitution, unknown-key rejection and HTTP authentication with disposable Nix stores and local test servers. The launcher and Linux integration suites also validate the generated VM cache configuration.
- `tests/ci-remote-worker.sh`: run Cards through hostd's Host RPC, mutual-TLS broker and packaged `tsugumi-workerd` on Linux; requires listeners and Nix builds. The script name describes the remote worker scenario.
- `nix develop -c /usr/bin/swift tests/tools/wkwebgpu.swift`: probe WebGPU and adapter availability in an offscreen WKWebView.
- `python3 sdk/codegen/scaffold.py go`: synchronize the host Go scaffold in `ui/shared/templates/go.json` after editing `sdk/go/template/` (`--check` verifies them).
- `python3 sdk/codegen/scaffold.py rust`: synchronize the host Rust scaffold in `ui/shared/templates/rust.json` after editing `sdk/rust/template/` (`--check` verifies them).

Scripted Mac trials can register a trusted root before the first project opens:

```sh
TSUGUMI_UI_TRUSTED_FOLDER=/absolute/trials ui/macos/tools/run-ui.sh dist/Tsugumi.app /absolute/trial.script /absolute/output /absolute/trials/project
```

Like the script's `setting` command, this registers a host setting, which remains until removed in Settings. Supply `TSUGUMI_HOST_STATE` to select an isolated host for a trial when needed. A direct scripted launch can use: `open -g -n --env TSUGUMI_UI_SCRIPT=/absolute/trial.script --env TSUGUMI_UI_TRUSTED_FOLDER=/absolute/trials --env TSUGUMI_HOST_STATE=/absolute/output/host-core dist/Tsugumi.app`. The trust input is ignored without a readable UI script and is never read from a project. Scripts and smoke runs leave the app in the background, including when an existing project window is selected.

## Execution ownership and timing

Actions and `ci-local.sh` use [ci/run-suite.sh](ci/run-suite.sh) for the same
command lists, gtest filters and pytest marker expressions. The canonical CI
owner of shared rules is **x86_64 Linux**. ARM Linux and macOS retain native
transport/client, locking, path, process, worker, ABI and sandbox checks. The
native exceptions are listed in [ci/run-suite.sh](ci/run-suite.sh) and
[ci/core_platforms.py](ci/core_platforms.py). Add a native exception when a
test exercises differing OS implementations. The static coverage check audits
the core pytest routes and the unfiltered CI runs of the C++ test targets.

| Checks | Full owner | Further targets |
| --- | --- | --- |
| Host settings and public Agent RPC | x86_64 Linux | Native gtest selectors for transport, locks, process lifecycle, credentials and OS adapters |
| Core RPC and remote results | x86_64 Linux | `native_core` pytest marker and native gtest selectors for scratch and shell tests |
| Real definition evaluation | x86_64 Linux | Native C++ dependency invalidation tests run on every architecture |
| Web type and behavior corpus | x86_64 Linux | Web/Wasm package build and packaged server/CLI smoke on ARM Linux |
| CLI/plugin/Web adapter corpus | x86_64 Linux | `package_smoke`: authenticated packaged server, Wasm policy, resident CLI startup |
| Python SDK pure libraries | x86_64 Linux | `not sdk_pure or package_smoke`; native plot/notebook import/render and child cleanup remain |
| SDK ABI/language runtimes, C# sandbox, runnable examples | Both Linux targets | Full native coverage |
| Swift and Seatbelt | macOS | Full native coverage |
| Real remote execution | x86_64 Linux | Separate worker admission/protocol and core-to-worker execution suites |

The canonical Agent suite also runs `tsugumi_input_types_tests` for resolver and
Host input descriptions, and `test_host_projects_rpc.py` for the Host's
project-copy permissions and state exclusions. Edit refusal and operation-ID
checks stay in `tsugumi_host_settings_tests`, and public edit/read-miss scenarios
stay in `tsugumi_agent_rpc_tests`. Short-identifier tests link the Host and core
in `tsugumi_host_settings_tests`.
Every native core suite runs all four `tsugumi_file_transfer_tests` scenarios
(`large`, `sparse`, `shrink-stream`, `shrink-sparse`). The SDK suite discovers
`tests/python/test_input_types.py`; core pytest discovers
`tests/core/test_short_identifiers.py`.

`tsugumi_worker_condition_tests` runs with the canonical core suite;
`tsugumi_definition_evaluation_tests` runs on every native target because it
validates embedded Nix dependencies and filesystem invalidation.

The local Linux pass represents the canonical full Linux job even on an ARM
Mac (`TSUGUMI_FULL_RULES=1`); its Mac pass omits shared rules. It runs the same
C# sandbox step and separate SDK example step as Actions, rather than collecting
these in the parallel SDK invocation. It also runs the real remote worker suite
on the available local Linux architecture (`TSUGUMI_LOCAL_REMOTE=0` explicitly
omits that separate integration job). `ci-local.sh agent` runs all Host rules
and public Agent transports on the local target. Local Nix builds use four cores
and one build job; local pytest uses two workers by default. Actions preserves
its automatic worker count through `TSUGUMI_PYTEST_WORKERS=auto`; the variable
can select a different local count without changing test ownership.

Use events, explicit gates or virtual time for test transitions. A watchdog
bounds a missing event; it is not a throughput assertion. Web background-tab,
renderer timeout and thumbnail idle/linger tests use virtual timers. Swift model
waits subscribe to Observation rather than exhausting a fixed number of ticks.
Real process/socket startup polls wait for an actual readiness record and retain
watchdogs. Deliberate performance checks are opt-in:

- `TSUGUMI_WIRE_BENCH=1 npm --prefix ui/web test` runs [bench/wire-frames.ts](bench/wire-frames.ts).
- `TSUGUMI_HEARTBEAT_BENCH=1 build-ci/bin/tsugumi-test-env python3 -m pytest -q tests/core/test_remote_recovery.py -k registered_heartbeat` enables [bench/heartbeat.py](bench/heartbeat.py).

The complete 340-case layout golden oracle remains in
`HostLayout.EveryGoldenRequestReturnsACorrelatedProposal`. Clients retain rich
wire specimens and their own proposal application, undo, staleness and retry
behavior. The core keeps one opaque-layout byte round trip; Host validation
keeps all invalid-layout vectors.
