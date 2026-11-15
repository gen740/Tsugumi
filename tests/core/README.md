# Core behavior tests

These scenarios exercise **the real core through its public RPC**. Read a test
as a condition, a user-visible operation, and its expected result. Low-level
framing, process ownership, value decoding and fault controls live in the harness.

```sh
nix develop -c cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
nix develop -c cmake --build build --target tsugumid-mock
nix develop -c python3 tests/run-isolated.py python3 -m pytest tests/core -q -m "not environment" --core-report
```

`--core-report` prints behavior statements with `PASS`, `FAIL`, or `KNOWN BUG`.
CTest also runs this suite as `core_rpc`:

```sh
nix develop -c python3 tests/run-isolated.py ctest --test-dir build -R '^core_rpc$' --output-on-failure
```

The socket-free `tsugumi_worker_staging_tests` executable exercises the production
scratch cleaner and declared-output copier with nested read-only Nix-style source
trees, links, FIFOs, hidden entries, open directories and a concurrent file writer.
It checks that link targets survive and errors identify the path and errno.
Output tests cover internal file/directory links, chains, escaping and absolute
links, cycles, FIFOs and concurrent link replacement; accepted copies contain no
symlinks or host bytes. Undeclared staging entries are ignored. CI runs it on
macOS and Linux; it can also be built without Nix or generated bindings:

```sh
c++ -std=c++20 -Wall -Wextra -Werror -pthread -I core/src -I workerd/include \
  workerd/src/staging.cc tests/core/worker_staging_test.cc -o /tmp/worker-staging-tests
/tmp/worker-staging-tests
```

`tsugumi_execution_service_tests` also tests both shared output-delivery modes
without a Card process, alongside native resident-transfer execution. See the
[worker boundary comparison](../../workerd/README.md) for the preserved behavior
of each carrier.

`tests/python/test_demo_marimo.py::test_notebook_passes_typed_result_and_keeps_snapshot`
additionally exercises the real Nix source modes, notebook subprocess, staged
Directory transfer, typed downstream result, retained HTML/images and snapshot
stability across a later run and daemon restart. The cleaner harness alone does
not cover those boundaries.

## A scenario

```python
def test_cancel_a_worker_waiting_to_return(core_factory, project, tmp_path):
    """Cancelling a waiting Card ends its run without publishing an output."""
    daemon = core_factory(mocked_builds=True, mocked_worker=True)
    client = daemon.connect()
    gate = Gate(tmp_path, "worker")
    worker_project(client, project, tmp_path, [gate.action(),
                   {"op": "output", "port": "out", "type": "string", "value": "too late"}])

    run_id = run(client)
    gate.wait()
    client.call("RunCancelRequest", runId=run_id)

    assert wait_run(client, run_id).state == RunState.cancelled
```

The gate establishes an order; an arbitrary sleep does not. Calls such as
`RunCancelRequest` stay visible because they are the public operation under test.
Fixture helpers are not another production API.

## What is real, and what is controlled

| Mode or control | Real code | Controlled dependency |
| --- | --- | --- |
| `core_factory()` | Installed/build-tree daemon and worker, all core services | None |
| `mocked_builds=True` | RPC, document validation, save/recovery, scheduler, history, worker pool and process handling | Nix build products |
| `mocked_worker=True` with mocked builds | The same core | A separate worker process speaking the worker protocol |
| `ReplyProxy` | Requests, core processing, notifications and unrelated replies | Delivery of a selected RPC reply |
| `fault=...` | Real persistence and recovery code | Existing opt-in daemon fault points |

`tsugumid-mock` is a **test-only executable**, installed only in `ci-core`. It links the same
core and replaces only the Nix evaluator. One private adapter receives the
confirmed definition directory; test cases do not import or call core classes.
The adapter does not parse internal Nix argument strings, interpret Nix, download
packages, or silently fall back to the real evaluator.

Use the real worker for shell steps, worker reuse and process/sandbox behavior.
Use the worker double for controlled outcomes, corrupt frames and uncooperative
processes. A mock build passing does **not** validate real Nix evaluation, its
watchdog, package construction, VM isolation, or platform-specific durability.
Those scenarios must use the real mode and suitable host fault controls.

## Writing fixtures

`Project(...).card(...).connect(...).save(client)` builds a project through public
save requests. Source updates obtain their base hashes through RPC. The shared
`projects.py` also prepares ordinary project files for existing scenarios and
external-edit/Reload cases. Neither helper reads private history or journal state.

Build plans are ordinary fixture sources named `mock-build.json`:

```json
{
  "manifest": { "run": { "bash": "true" }, "shell": "/bin/bash", "path": "/usr/bin:/bin" },
  "gate": "build-ready"
}
```

`manifest` uses the actual build-product format (`run.bash` or `run.package`,
the `shell` and `path` of the Card's processes, optional `preRun`/`postRun`);
`Project.card` fills in the host's `/bin/bash` and `/usr/bin:/bin` when a fixture
does not name them. `closure` adds paths to the recorded environment. `error` produces a build failure. `gate` pauses before
the product is returned; use `Gate(daemon.gates, name)` to wait/release it.
`cached` can explicitly control the external cache result.

A mock worker package contains `scenario.json` with `startup` and `actions` lists.
Startup actions run before the worker begins reading. Normal actions include:

| Action | Behavior |
| --- | --- |
| `output` | Emit a typed one-field value; choose `port`, `type`, and `value` |
| `echo` | Forward an input's bytes to an output |
| `file`, `directory` | Create output data and emit its path |
| `gate` | Announce arrival and wait for release |
| `hang` | Wait indefinitely; optionally ignore cancellation |
| `failure`, `exit`, `malformed` | Fail, terminate, or violate the worker protocol |
| `log`, `display_text` | Publish logs or a display |

All waits in the harness are bounded. Worker doubles notice owner disconnection
even when ignoring cancellation. Daemon logs are attached to failing pytest reports.

## Regression and migration policy

- Assert RPC replies, failures, notifications, recorded values and artifacts.
- Do not assert core function calls, private class layouts, SQL rows, journal
  filenames, storage directories, or diagnostic implementation wording.
- External project edits and mock controls are setup, not evidence that core did
  the right thing. Obtain that evidence through RPC.
- Known defects have narrow, **strict** `xfail` marks and desired-behavior
  assertions. A fix becomes an unexpected pass until its mark is removed. To see
  the actual failures, run `pytest tests/core --runxfail --core-report` in the
  development environment. Do not mark a new failure expected without diagnosis.
- Client/Agent/UI bugs are not core bugs. This suite checks the core contracts
  they consume (for example, a completed operation can contain a failure receipt).
  It cannot establish that a client renders or interprets that receipt correctly.

Real Nix and host sandbox tests live in `integration`, marked `environment`.
CTest's `core_rpc` runs the deterministic suite; CI runs the environment suite
separately with:

```sh
nix develop -c python3 tests/run-isolated.py python3 -m pytest tests/core/integration -q --core-report
```

## Card definition evaluation

`integration/test_definition_evaluation.py` uses the embedded Nix evaluator and
real workers. It compares stdout, recorded environment identities and worker
reuse after an input edit and reversion. SDK cases edit a private copy at the
same path, both with a resident daemon and after restart. A second target
system requires an advertised runnable system; that case skips otherwise.
The resident SDK cases are strict xfails: the next run retains the old stdout
or view bundle, environment identity and resident worker. Restart cases pass;
the defect concerns a mutable SDK path, not an immutable Nix store deployment.
Flake cases serve pinned fixtures over local HTTPS and need Nix, openssl and
downloads. The second nixpkgs pin is fixed, never resolved from a moving branch.

`test_definition_evaluation_economy.py` counts complete evaluator entries through
the existing test-only Nix adapter. Its strict xfails specify cross-run reuse
of full Card evaluation; the control distinguishes a store hit from skipped
evaluation. A fresh execution still has to happen: the resident worker double
reports invocation counts 1 then 2. No test reuses a previous run's data or uses
elapsed time to infer evaluation counts.

The counter assumes each entry to `NixEvaluator::evaluate` performs full Card
evaluation. The real implementation applies and forces the entry and manifest
there, including on ordinary repeated Run requests. Build-cache flags and stage
durations cannot distinguish this work. The adapter publishes its integer before
returning, so the completed run RPC is the reader's barrier. This instrument
detects repeated evaluation; it does not prevent it or measure Nix's internal
thunk forcing. If the evaluator later splits full and light operations, move
the counter to the full operation. No production API or behavior is added.

Restricted Card evaluation is not `pureEval`: `getEnv` is masked and outside
reads are rejected, but `currentTime` and `currentSystem` remain available.
The economy cases therefore do not establish that arbitrary Card Nix is pure,
or justify a cache policy for those ambient inputs. The SDK path is also a real
evaluation input; a version-number-only edit is not an artificial cache key.

Build the real daemon/worker and updated double, then run from the checkout:

```sh
nix build -o ./build-ci .#ci-core
./build-ci/bin/tsugumi-test-env python3 -m pytest -q --core-report \
  tests/core/test_definition_evaluation_economy.py \
  tests/core/integration/test_definition_evaluation.py
# Diagnose strict xfails instead of hiding their assertion failures:
./build-ci/bin/tsugumi-test-env python3 -m pytest -q --runxfail \
  tests/core/test_definition_evaluation_economy.py
./build-ci/bin/tsugumi-test-env python3 -m pytest -q --runxfail \
  tests/core/integration/test_definition_evaluation.py -k 'sdk and resident'
```
