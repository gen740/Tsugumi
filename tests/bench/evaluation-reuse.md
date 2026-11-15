# Definition evaluation reuse

`tsugumid` retains a successful `NixResult`, not a Card invocation's result.
Every Run still invokes the Card. This addresses the measured repeated full
manifest evaluation, rather than caching the much cheaper manifest decoding.

The explicit key is the nixpkgs URL (including its revision), narHash, target
system, and exact entry arguments. Those arguments reference content-addressed
Card sources (including locks), generated contracts and Nix parameters, and the
transitive upstream definition graph. Generated `sources.json` contains each
resolved project flake's locked ref/revision/narHash and package products after
its approved overrides. Native/remote mode is an entry argument. Runtime
parameter values, labels, descriptions and layout do not enter this key.

The entry implementation and evaluator settings belong to one evaluator epoch.
The epoch also owns the union of observed filesystem reads, so previously cached
imports and lazy nixpkgs values cannot hide earlier dependencies. File contents,
symlink targets, directory membership/types and negative existence checks are
validated before reuse. File content digests are memoized behind device, inode,
mode, size, nanosecond mtime and ctime checks. This notices mutable SDK content
at the same path, including sources copied into the store. A change replaces
the Card EvalState (including import/source-to-store caches) and invalidates
entries from its old epoch. The flake evaluator's referenced settings remain
alive. `getEnv` is empty in restricted mode; the initial `currentTime` constant
is retained across those state replacements. Both were checked against the
installed Nix 2.34.9 source and by an embedded-evaluator test.

Store objects are immutable and observed objects receive temporary roots.
Every hit roots and validates the derivation and manifest output. Missing paths
cause a miss and a fresh import state. Transport, native realization checks,
closure computation, permanent-root replacement/registration, manifest parsing,
environment identity calculation, execution limits, cancellation and deadlines
remain per-Run operations. Remote outputs absent from the local store cannot
hit this cache; they are evaluated again, as required by the validity rule.

Only hashed fetchurl/fetchTarball reads are eligible. Other fetchers, getFlake,
exec/importNative, untracked physical filesystem access and observation-limit
exhaustion disable reuse. Such evaluations still run. No known class of
unobserved reads is accepted. As with evaluation itself, in-place SDK changes
concurrent with preparation do not have atomic snapshot semantics: the caller
must not mutate SDK files while their input check/evaluation is in progress.
The metadata fast gate assumes normal filesystem change timestamps and Nix's
store immutability; it is not a guarantee against externally corrupted storage.

The cache retains one latest entry per Card, at most 256 Cards; edits replace
entries, deleted Cards are pruned on the next preparation, and obsolete epochs
are discarded. This bounds history growth during repeated editing. Each
observation collection is capped at 8,192 items; hitting its cap disables reuse.
There is no evaluation-cache file or run-result retention policy.

## Validation on this Mac

Release CMake builds used `-Wall -Wextra -Werror -Wformat=2
-Wimplicit-fallthrough -Wmissing-designated-field-initializers`.
`nix build -o ./build-ci .#ci-core` was attempted. The later Nix source snapshot
for the tests at `/nix/store/2r2b0jxghqba50g8li32m64ih0n9q5d0-source` contains
the changed CMakeLists but omits the untracked new C++ test. The final Nix
test-package build failed with `Cannot find source file:
definition_evaluation_test.cc`; its production-core dependency built. The worktree CMake
build therefore supplies validation without staging files. No commit was made.

- Embedded evaluator: 8 passed, covering explicit key components, SDK imports,
  copied SDK trees, directory additions/removals, target systems, missing
  derivations/outputs, masked environment and constant time after refresh.
- Evaluation economy and key changes: 24 passed, including the waiting branch's
  12 economy cases with its pending decorator temporarily removed, and 12 local
  input-key cases (including independent flake ref/narHash/override changes). These check
  fresh invocations as well as evaluator counts. The temporary copies were removed.
- Focused economy/key/environment tests: 47 passed; the additional upstream
  invalidation case passed separately. The existing closure assertion caught
  and verified a fix to retain the double's supplied closure on a cache hit.
- Full non-environment core run: 437 passed, 23 failed, 4 existing xfailed.
  Failures are read-only shell sandbox rejection and 22 display cases whose
  native worker exits in this nested sandbox. Assertions were not weakened.
- Existing C++ core tests: 67 passed, 4 existing sandbox-dependent skips.
- Static checks: 66 passed.
- The large remote log test passed on retry. The real remote system test reaches
  assignment, but its `nix derivation show` assertion expects a different JSON
  shape from this Nix CLI (and the isolated CLI needs nix-command enabled).
  The new C++ system test checks actual evaluated derivations without that CLI.
- Full Card correctness integration cannot complete here because the native
  worker cannot enter its nested sandbox. The caller must run the waiting
  branch's `tests/core/integration/test_definition_evaluation.py`.
- The isolation wrapper reports failure after the tests because this sandbox
  refuses `ps` during service discovery/cleanup. Test-body results above are
  separate from that wrapper exit status. No application was launched and no
  application-support or credential data was accessed.

The waiting branch should remove `@PENDING_REUSE` from
`test_non_derivation_edits_do_not_repeat_full_evaluation`, then remove its unused
`PENDING_REUSE` definition. Also remove the `restart=False` strict-xfail mark
from `test_sdk_deployment_changes_the_next_definition` (both SDK input cases).
Keep all assertions.

## Measurement

The ordinary `tests/bench/run.py` cannot finish a scene here because the worker
exits. A temporary probe `/private/tmp/bench-evaluation.py` uses that file's
`make_project(..., "value-resident-50")` and `measure_stages.collect`, issuing
50 separate Runs to one daemon and retaining completed evaluation-stage times
even though execution fails. It does not substitute a worker or change the
benchmark Card. Both probes run through `build-ci/bin/tsugumi-test-env`.

The repeat baseline at `/private/tmp/evaluation-before-repeat.json` measured
124 ms warm median (49 Runs), 125 ms median including the first Run. The old
binary has no full-evaluation counter. An earlier baseline measured 102 ms warm
median. Store SQLite contention and concurrent work make cold timings noisy;
these are evaluation-stage measurements, not end-to-end execution timings.

The final probe at `/private/tmp/evaluation-after-complete.json` measured
**3 ms median** both warm and including the first Run, with **one full evaluation
for 50 Runs**. Its cold evaluation was 5,961 ms; the baseline repeat's cold
value was 30,904 ms. The Release daemon SHA-256 was
`001836a29e04d40b6aa9c77b78c740208410ed93dec50bbbd4ef9bd6fbe9688b`.
The warm comparison is 124 ms to 3 ms (about 97.6% less evaluation time);
the 50-Run medians are 125 ms to 3 ms. Failed worker execution is excluded
from these stage intervals, but remains visible in the raw probe rows.
