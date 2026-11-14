# Repository tests

Run commands from the repository root. Build instructions are in [Installing and building](../docs/INSTALL.md#building-from-source); [core/README.md](core/README.md) gives the core suite's detailed workflow.

Run `tests/check-fast.sh` before every push: the static tests (`tests/static`: tests that only read files of the repository, which CI runs first in its own job) and the Web UI Nix check, including the generated demo player (`--no-nix` skips it). Run the platform checks with `tests/ci-local.sh`. On macOS this runs the Mac checks locally and the Linux checks in Docker or OrbStack. Use `tests/ci-local.sh mac` or `tests/ci-local.sh linux` for one platform, and `tests/ci-local.sh agent` for the public Agent RPC fixtures.

The CI definitions are [ci.yml](../.github/workflows/ci.yml) and [ci-platform.yml](../.github/workflows/ci-platform.yml).

| Location | Coverage |
| --- | --- |
| [tests/core/](core/README.md) | Core behavior, public RPC and integration tests |
| `tests/python/` | SDKs, examples, demos, CLI/plugin/Web adapters and host/VM integration |
| `tests/static/` | Repository contracts, packaging, release, licenses and generated-file consistency |
| `tests/remote/` | C++ remote worker admission, scoping, transfers and recovery on Linux |
| `macOS/CoreTests/`, `macOS/Tests/` | Agent, RPC and app behavior |
| `web/` | Browser UI type checks and behavior contracts |
| `tests/prompt/` | Offline prompt probe scoring |

The build generates C++/Python/Swift/TypeScript bindings and BFBS with `flatc`; these stay in build outputs and are not committed.

CI runs deterministic core RPC and Agent tests on Linux and macOS. SDK and Web checks run on Linux; Swift and Seatbelt checks run on macOS. Real remote-worker execution runs on x86_64 Linux through `tests/ci-remote-worker.sh`. The socket-free thought scheduler test is part of `tsugumi_host_settings_tests` in the Agent RPC job.

The large demo runs are opt-in; CI collects them but leaves `TSUGUMI_TEST_DEMO_RUNS` unset. Run them explicitly in the development environment:

```sh
TSUGUMI_TEST_DEMO_RUNS=1 nix develop -c python3 -m pytest -q tests/python/test_demo_*.py
```

The shared-VM suites also require an explicit opt-in on an Apple silicon Mac with the VM image installed. Build the signed controller first, then run:

```sh
macOS/tools/build-vmctl.sh
TSUGUMI_TEST_CONTAINER=1 nix develop -c python3 -m pytest -q \
  tests/python/test_shared_vm.py tests/python/test_cards_container.py tests/python/test_workers.py
```

`TSUGUMI_TEST_VM_SUPPORT` selects the persistent test VM support directory (default: `build/test-vm`); `TSUGUMI_VM_X86=qemu` selects QEMU instead of Rosetta for x86_64 Linux Cards. These VM suites do not run in CI.

`tests/core/agent/environment_rpc_test.cc` exercises Host environment requests against agentd and an external fake `tsugumid` with authenticated project RPC. It covers readiness/registration, concurrent starts and aliases, process death and retry, resident-process adoption, explicit history recovery, folder withdrawal, stop and broker capability revocation. `environment_adapter_test.cc` supplies a narrow helper double to the same lifecycle service. The RPC scenarios use Unix sockets; the helper-double scenarios run without sockets. Neither launches a VM or invokes Nix. Build `tsugumi_agent_rpc_tests` and `tsugumi_host_settings_tests`; run them with `--gtest_filter='EnvironmentFixture.*'` and `--gtest_filter='EnvironmentAdapterTest.*'`, respectively. The CLI start/origins/failure scenarios in `tests/python/test_cli.py` run the Web host through agentd against `vm_helper_double.py`, a private FlatBuffers adapter with a native project daemon. `test_web_server.py` verifies environment residency and stop/restart through the same Host requests. `web/tests/environmentHost.ts` and Swift `ServiceWireTests` check request encoding, events and omitted vectors without sockets. Mount selection and folder withdrawal are tested with the C++ helper double; Swift keeps no second mount-selection implementation.

`tests/core/agent/guide_rpc_test.cc` checks both embedded guide languages, script completeness and demo targets, interests and choices, fallback replies, failure/retry/change/farewell, late and duplicate run observations, two windows sharing a guide, progress scope and restart/resume. Build `tsugumi_agent_rpc_tests`; run `--gtest_filter='GuideScript.*:GuideFixture.*'`. These scenarios need sockets but no model or Nix daemon. Native bird geometry stays in `macOS/Tests/TsugumiAppTests/GuideTests.swift`; Web and Swift wire tests cover omitted guide vectors and presentation adapters.

Version notes have socket-free tree and localization tests in `tests/core/version_note_test.cc` (`tsugumi_remote_result_tests --gtest_filter='VersionNote.*'`) and settings validation in `tsugumi_host_settings_tests --gtest_filter='HostSettingsStore.VersionNotes*'`. `tests/core/agent/version_note_rpc_test.cc` exercises the human-only note request, core model selection, concurrent windows, persistence across restart, mode switching, failure/retry and empty diffs (`tsugumi_agent_rpc_tests --gtest_filter='AgentProjectFixture.VersionNote*'`). Those RPC scenarios need Unix sockets and a local model fixture. `web/tests/versions.ts` checks the presentation request, settings, retries and late responses without a browser.

For documentation changes, run `nix run .#build-doc` and `nix run .#test-doc`. These commands use the shared generated demo player (`nix build .#site-player`); Web checks also test its generated data. Without Nix, site and Web builds and Web tests generate the player from `web/node_modules` and `web/src/gen`.

## Manual tools

- `python3 macOS/tools/test-binary-cache.py`: check signed substitution, unknown-key rejection and HTTP authentication with disposable Nix stores and local test servers. The launcher and Linux integration suites also validate the generated VM cache configuration.
- `tests/ci-remote-worker.sh`: run Cards through agentd's Host RPC and mutual-TLS broker on Linux; requires listeners and Nix builds.
- `nix develop -c /usr/bin/swift tests/tools/wkwebgpu.swift`: probe WebGPU and adapter availability in an offscreen WKWebView.
- `python3 sdk/codegen/scaffold_go.py`: synchronize the Swift and TypeScript Go scaffolds after editing `sdk/go/template/` (`--check` verifies them).
- `python3 sdk/codegen/scaffold_rust.py`: synchronize the Swift and TypeScript Rust scaffolds after editing `sdk/rust/template/` (`--check` verifies them).
