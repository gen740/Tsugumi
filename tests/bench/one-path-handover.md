# Local workerd handover: implementation and measurement

This worktree implements execution-path step 1. Local execution always borrows a
connection from `tsugumi-workerd`; the direct-start core pool is removed. The
remote adapter is unchanged apart from calling the shared invocation engine with
a release callback. The branch already contained per-registration heartbeat
settings (15 seconds, zero missed = never) and core-lifetime interruption.

This is **not completion of all four requested steps**. Grade registration,
condition-based worker selection, UI entry/recording, unified timeout budgets
and visible transfer progress remain. Remote workers are still one-shot; remote
resident slots are not implemented by this local handover change.

The ownership change also needs a failed borrowed lease to be returned when
its caller unwinds. This is a real path: the existing transfer/invalid-output
tests exercise exceptional exits. Closing borrowed descriptors alone would
leave workerd holding the lease. The borrowed connection's destructor closes
its channels and asks workerd to discard that lease; a unit test checks the
ordering. This preserves cleanup, not run recovery.

## Checks run

- Staged-copy `nix build --cores 4 --max-jobs 1 ...#ci-core`: passed; no compiler
  warnings. The core/test C++ sources in this tree match the built snapshot.
- `tsugumi_execution_service_tests`: 25 passed, including five local handover
  checks and both real unsandboxed resident transfer-failure cases.
- `tsugumi_remote_result_tests`: 72 passed, four existing Seatbelt tests skipped
  because the outer sandbox refuses Seatbelt creation.
- `tsugumi_agent_rpc_tests --gtest_filter='BrokerFixture.*'`: 13 passed,
  including per-registration heartbeat persistence and delivery to the core.
- `tsugumi_workerd_tests` and `tsugumi_worker_staging_tests`: passed.
- `pytest -q -m 'not environment' -n 2 tests/core`: **478 passed, 23 failed,
  4 xfailed**. The new local service's three cases passed. Failures are 22
  marimo/display cases in `test_rpc_values.py` (worker exits before returning;
  two then have no display JSON), and
  `test_project_shell.py::test_shell_read_only_is_explicit[False]` (Seatbelt
  command exits 71). The shell failure and the representative display case
  `[double-rounding-tail]` were reproduced against the original binary and
  original test sources. This is not a green full Python suite.
- `bash tests/check-fast.sh --no-nix`: 75 static tests passed.
- Public document/prompt generators were run; no checked-in public bindings or
  UI strings changed. No Swift or Web implementation changed.

Every isolated test command also reported `ps: Operation not permitted` during
runner cleanup, making its overall exit status 1 even when its assertions
passed. No skip or assertion was added to hide these failures. Full logs are in
`/private/tmp/one-path-*-tests.log` and `one-path-static-final.log`.

## Measurement

2026-11-16, native Darwin Release build on the shared Mac while other builds were
running. No power setting was changed. The normal sandbox-enabled measurement
failed before producing samples (`the worker exited before returning a result`):
`/private/tmp/one-path-before.json` and `/private/tmp/one-path-after.json`.
This does not establish sandbox-enabled
execution or performance.

For a diagnostic comparison, the same `tests/bench/run.py` small Python Cards
were generated with `sandbox: false` on **both** binaries. Only the benchmark
fixture setting was overridden; no test assertion or product behavior was
changed. Each measurement used three fresh-daemon bundles; resident bundles had
50 sequential Run requests, with the first invocation included by the tool.

| Metric (median ms) | Original | Handover | Original recheck after handover |
| --- | ---: | ---: | ---: |
| Fresh Run RPC/inv | 958.88 | 3413.48 | 8518.29 |
| Fresh evaluate | 780 | 3124 | 8279 |
| Fresh execute | 169 | 283 | 196 |
| Resident RPC/inv, bundle average including first call | 35.84 | 105.30 | 322.08 |
| Resident invocation wall | 3 | 8 | 15.5 |
| Resident execute | 1 | 2 | 1 |
| Resident individual Run RPC, calls 2–50 only | 13.54 | 14.35 | 26.41 |

The observed warm resident difference in the first comparison is **+0.81 ms per
Run**. It is not an established handover cost or a passed performance gate: the
unchanged original binary subsequently slowed substantially too. Fresh timings
are dominated by evaluation, which precedes requesting a local worker. Neither
load nor cache effects can be isolated from these samples. Remeasure in quiet
conditions with the normal sandbox before accepting performance thresholds.
Do not compare these results with the earlier low-power measurements.

Raw JSON (all samples, stage timings, environment, binary hash and retained
benchmark directories):

- `/private/tmp/one-path-before-unsandboxed.json`
- `/private/tmp/one-path-after-unsandboxed.json`
- `/private/tmp/one-path-baseline-recheck.json`

Original daemon SHA-256:
`133e51b3f2aa940474712846a34f3431855ccddcb6e16f37603d533fa7bbcc7c`.
Handover daemon SHA-256:
`6a7f1bd275216ba0a9872cbe195dba2df65c75720523b178e381cf8c1dce2adc`.

The diagnostic command uses the ordinary benchmark runner with this explicit
fixture override (use the corresponding binary/SDK/bindings environment for
each revision):

```sh
./build-ci/bin/tsugumi-test-env python3 -c '
import sys
sys.path.insert(0, "tests/bench")
import run
run.SCENARIOS = ("value-fresh", "value-resident-50")
original = run.python_card
run.python_card = lambda *a, **k: dict(original(*a, **k), sandbox=False)
provenance = run.provenance
run.provenance = lambda *a: dict(provenance(*a), worker_sandbox=False)
raise SystemExit(run.main())
' --build-type Release -n 3 --out /private/tmp/local-handover.json
```

## Outside-sandbox checks still needed

Use a build of this worktree, with matching SDK and bindings. These commands
retain the existing assertions and do not exclude the owner's known failures.

```sh
# Native sandbox, loading, PATH, resident retention, large sends, files and deadlines:
./build-ci/bin/tsugumi-test-env python3 -m pytest -q -n 2 \
  tests/core/integration/test_resident_workers.py \
  tests/core/integration/test_invocation_isolation.py \
  tests/core/integration/test_worker_loading_boundary.py \
  tests/core/integration/test_host_isolation.py
./build-ci/bin/tsugumi-test-env python3 -m pytest -q -n 2 tests/core/test_worker_loading.py
./build-ci/bin/tsugumi-test-env python3 -m pytest -q -n 2 \
  tests/core/test_rpc_values.py \
  'tests/core/test_project_shell.py::test_shell_read_only_is_explicit[False]'
./build-ci/bin/tsugumi-test-env ./build-ci/libexec/tsugumi-tests/tsugumi_execution_service_tests

# Normal sandbox-enabled measurement, without the diagnostic fixture override:
./build-ci/bin/tsugumi-test-env python3 tests/bench/run.py \
  --build-type Release -n 5 --out /private/tmp/local-handover-sandboxed.json

# On Linux with namespaces/dependencies available:
./build-ci/bin/tsugumi-test-env ./build-ci/libexec/tsugumi-tests/tsugumi_remote_result_tests
CORE_TEST_ENV="$PWD/build-ci/bin/tsugumi-test-env" bash tests/ci-remote-worker.sh
```

Also check real VM start/stop and worker ownership there. No Tsugumi.app was
launched, no foreground was taken, and no real application support folder or
credentials were accessed in this task.
