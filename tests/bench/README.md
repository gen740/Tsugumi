# Manual execution baseline

These are operator-run measurement tools, restored from the two probes removed
by `58079032` and updated for the current Card inputs/report/result, ports and
public RPC. They are not pytest tests, are not named `test_*.py`, and are not
registered with CTest or CI. No product implementation, cache, worker ownership,
retention policy or execution setting is patched by these tools.

The baseline is for the later move to a single workerd path. It measures today's
local path and, optionally, today's remote path. It does not implement that move.

## Build and run on macOS or Linux

Run from the repository root, with a working Nix installation and the repository's
development dependencies. Python needs `flatbuffers`; the CMake build generates
the matching object-API bindings. Use the same build type for every comparison.
For example, on either native Darwin or Linux:

```sh
nix develop -c cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
nix develop -c cmake --build build --target tsugumid tsugumi-worker tsugumi_protocol_gen
nix develop -c python3 tests/bench/run.py --build-type Release --repetitions 5 --out /tmp/local-baseline.json
```

macOS uses the native Darwin daemon and worker. No app, foreground interaction,
VM controller or shared Linux VM is needed. Linux needs the sandbox facilities
and dependencies described in [building](../../docs/INSTALL.md#building-from-source).
Run as an ordinary user on a disk-backed filesystem. The generated benchmark
Cards use `sandbox: true` and `recordIO: false` on both paths.
Only the resident scenario sets `reuseWorker: true`.

The existing `tests/core/harness/daemon.py` starts a real `tsugumid`, with its
ordinary worker and Nix implementation. Each repetition has a new project and
daemon. The harness fixes two daemon CPUs; requests use `maxConcurrency: 1`, and
the default one Nix job/core. There are no hidden warmups, sleeps between calls,
cache-clearing operations, mock builds or replacement workers. Daemon startup
and project opening are outside invocation timing. Existing Nix store contents,
OS caches and worker prefetch remain in effect: "first" does not mean a cold
machine or an empty store. Local runs precede remote runs.

For another build tree, set `TSUGUMI_BUILD_DIR` before starting Python. The harness
also accepts `TSUGUMID`, `TSUGUMI_WORKER`, `TSUGUMI_SDK_DIR`, and
`TSUGUMI_PY_BINDINGS` (normally `$TSUGUMI_BUILD_DIR/generated/python`). Bindings,
daemon, SDK and worker must match this checkout. `TSUGUMI_WORKER` selects the
harness's `--worker` fallback; a worker supplied by a Card's built environment
still follows the normal product selection rules.

### Remote comparison

Supply an already-running remote-worker endpoint and an already-running Host
(`tsugumi-hostd`). The script starts neither of those services. Configure mutual
TLS first as in [Remote execution](../../docs/RemoteExecution.md): the worker must
already trust **this Host's** public key. The worker's public pin is also required.
Use a Host from the same revision; a separate benchmark Host avoids unrelated
activity affecting measurements. Its `host.ready` contains the authenticated
Host socket and broker endpoint. For example:

```sh
nix develop -c python3 tests/bench/run.py --build-type Release -n 5 \
  --remote-endpoint tls://worker.example:8765 \
  --remote-worker-key 'ed25519:PASTE_COMPLETE_WORKER_SPKI' \
  --remote-host-ready /absolute/path/to/host-state/host.ready \
  --out /tmp/local-and-remote-baseline.json
```

A TLS Unix listener (`unix:/absolute/path/to/worker.sock`) is also accepted.
The tool uses the existing harness Host wire client to create a unique `bench-*`
service registration, verify the pin, and authorize only its new benchmark
projects. It removes that registration and revokes those project capabilities at
exit; it does not change existing service registrations. The existing Host
broker transports jobs to the supplied worker. The measured invocation records
must contain the requested service ID and remote job IDs. No local fallback is
used. Remote capabilities and server ID are recorded, but the worker does not
expose its build type: record that separately alongside the report.

**Current remote resident limitation:** `tsugumid/document/src/graph.cc` rejects a remote
`reuseWorker` Card, and remote execution reports fresh workers. The remote
resident row is therefore explicitly `unavailable`, with no samples, rather than
substituting fresh calls. Measuring it requires remote resident slots and a reuse
contract, outside this measurement-only step. All other remote scenarios execute
normally, including a real 100-implementation run list.

## Fixed scenarios

`-n N` repeats each bundle N times (default 5). All invocations are serial.

| Scenario | Work per repetition |
| --- | --- |
| `value-fresh` | One Python Card returning one small integer, with a fresh worker state |
| `value-resident-50` | The same operation on the same Card, 50 sequential Run requests to one daemon; the first call is included |
| `file-1gib` | One File output, exactly 1,073,741,824 bytes, written in 1 MiB blocks |
| `file-sparse-1gib` | One File output of the same logical size, created with truncate; source allocation is recorded |
| `directory-10000` | One Directory output containing 10,000 files of 128 bytes each |
| `file-input-1gib` | Read and SHA-256 a complete 1 GiB File input; one upstream producer/setup run supplies saved inputs, then the measured request explicitly reruns only the consumer |
| `run-list-100` | One definition whose Nix `run` is a list of 100 short Python implementations, invoked in one Run request with fresh worker state |

The input scenario's producer, initial consumer and saved-input setup are recorded
under `bundles[].setup` and excluded from measured samples. It intentionally
benchmarks replay of the saved File input, including ordinary input staging.
File generation uses buffered writes without fsync; it is not a disk durability
benchmark. Sparse source allocation does not imply sparse-preserving hand-over.
The small report emitted by each benchmark Card adds the same instrumentation
path across comparable builds; its own encoding and report publication are
outside `run_ms` but inside the invocation.

## Reading the table and JSON

One table is printed and one JSON report is written to `--out`. The JSON retains
every request, invocation, stage timestamp/outcome, build-cache and worker-reuse
flag, fixture metric and repetition membership. Failed scenarios are labelled,
never treated as zero-time successes, and make the command exit nonzero. The
unsupported remote resident row alone does not make it fail.

Table time cells are **median / nearest-rank p95, in milliseconds**. `n` counts
measured invocations: N for ordinary scenarios, 50N for resident, 100N for the
run list. Invocation statistics pool those samples; per-bundle request wall time
is separately normalized by invocation count and summarized over N repetitions
in `request_wall_ms_per_invocation` (`RPC/inv` in the table). JSON includes
sample counts and totals; for N=1, median and p95 coincide.

| Number | Source and meaning |
| --- | --- |
| `RPC/inv` | Python `perf_counter_ns`, from sending RunRequest until terminal RunStatus is observed, divided by invocation count per bundle; includes RPC and up to one 10 ms polling interval, excludes history reads |
| `wall_ms` | Latest recorded stage end (or endedAt, whichever is later) minus startedAt; per-invocation core wall time, includes recorded save work, excludes queue wait and the final history commit |
| `queue_ms` | `startedAt - readyAt` from RunInvocationsRequest: wait for a core lane after inputs were ready; not the remote worker's queue |
| `prepare_ms`, `evaluate_ms`, `build_ms` | Sums of the corresponding StageTime intervals, with original outcomes retained; reused/cached zero-length stages remain zero; absent stages are null |
| `execute_ms` | Existing execute StageTime; a coarse envelope, including more than the Card body |
| `worker_call_ms` | Already emitted execute RunTimeline end minus start; on local code Cards this covers worker.call, including SDK work; it excludes acquisition/input staging and later output copies. Events may be coalesced, so availability/sample counts can differ. Remote has no corresponding event |
| `validate_ms`, `save_ms` | Recorded output validation and save StageTime intervals; save closes just before the completion transaction, and does not time its final commit |
| `run_ms` | Monotonic duration inside benchmark main, including file writes/reads but excluding SDK encoding and report publication; emitted through Report and read from RunRecordRequest |
| `output_logical_bytes`, `output_allocated_bytes` | Benchmark-generated file sizes and `st_blocks * 512` before worker hand-over (Directory sums file entries, not directory metadata); JSON only |
| `input_read_bytes` | Bytes actually read by the consumer's benchmark body, not bytes staged by the core; JSON only |
| `Card PIDs` | Number of distinct local PID values reported by measured Card bodies, not a spawn counter; PID recycling is possible. Remote namespace PIDs are retained in JSON but are not counted as distinct host processes |
| `daemon_processes_started` | Exactly one harness-launched tsugumid for each started bundle; JSON only, outside invocation time |

The original `durationMs`/`endedAt` are retained too. They stop before the save
stage, so `wall_ms` uses stage ends as well. These stage envelopes are **not an
additive decomposition of RPC time**. Queue overlaps preparatory work in some
execution shapes; list preparation can be charged to its first invocation. On
remote, core evaluate/build describe host preparation/export, while remote
realization, remote queue, polling and result import occur within broader
existing stages. Do not compare a remote `build_ms` with local `build_ms` as if
both captured all Nix work on their respective execution hosts.

### What cannot currently be separated

The JSON `unavailable` section names each missing metric and the smallest
instrumentation change that could expose it. `null`/`-` means unavailable,
never zero. There are no estimated startup, hand-over or copy numbers.

- **Start:** no persisted spawn/acquire-to-ready timestamps. Add durations around
  those existing boundaries to the invocation summary.
- **Full worker invocation:** `run_ms` measures the benchmark body, while the
  full worker-side invocation including SDK work has no persisted duration.
  Return that duration in the result and store it, avoiding subtraction of clocks
  from different machines. The local worker-call envelope remains a broader
  observation that also includes host-side RPC handling.
- **Remote queue/build:** core queue/build stages do not isolate remote waiting
  and Nix realization. Return those worker-clock durations in the remote result
  and persist them alongside the existing host stages.
- **Output hand-over:** local copying, validation/cleanup and remote import have
  no standalone recorded interval. Time the existing copy/import boundary and
  persist the duration.
- **Bytes copied:** logical size and source allocated blocks do not count physical
  copying, reflinks, archives, deduplication or retries. Count successful bytes in
  existing copy and blob-transfer loops, with input/output directions separate.
- **Processes spawned:** resident reuse flags do not count helpers, Nix, remote
  supervisors or standby refill. Even `workerReused: false` may consume a worker
  prefetched earlier by WorkerPool. Count successful spawn calls, including
  standby refill, and return per-invocation/build counters. Reported Card PIDs
  only show processes that actually reached the benchmark body.

These omissions are evidenced by the current Phase/StageTime/InvocationSummary
contracts and execution-service boundaries; no product instrumentation is added
in this step. They prevent an honest finer split, not collection of existing
wall times and coarse stages.

## Comparability and retained data

**Numbers are comparable only on the same machine and build type.** For a remote
comparison that means the same client machine, same remote execution machine,
same network and same build type on each side across baseline/candidate runs.
Keep the repository revision, SDK, compiler options, sandbox configuration,
filesystem, cache state, power/load and free disk space comparable. Darwin-local
versus Linux-remote also changes OS, architecture and isolation; this is not a
controlled measurement of transport overhead alone.

The report records the daemon binary SHA-256, build type supplied by the operator,
revision/dirty state, platform, SDK/worker paths and remote identity. The actual
Card environment ID is available in each invocation. Keep the JSON as the raw
baseline; it is an observation, not a performance threshold or CI assertion.

Projects, history, daemon logs, scratch and output data remain on disk. The JSON
records the workspace and each daemon directory (including failures). Repeated
large outputs and copies can consume many GiB. The tool stops only the daemons it
started; it does not delete run results. Review those paths and remove them
manually when finished. An interrupted remote job may remain recoverable in the
worker journal under its ordinary product lifecycle; inspect it before revoking
or removing abandoned state.

## Restored standalone probes

```sh
nix develop -c python3 tests/bench/measure_stages.py collatz --out /tmp/stages.json
nix develop -c python3 tests/bench/measure_stages.py builtin-chain --out /tmp/chain.json
nix develop -c python3 tests/bench/measure_stages.py large-values --out /tmp/values.json
nix develop -c python3 tests/bench/measure_language_workers.py --languages r jl --out /tmp/languages.json
```

The stage probe runs a first pass and rerun separately, on native Darwin or Linux.
`--project /absolute/project --start card/path` can probe another current-format
project. `large-values` retains the original 100,000-element Python → C++ → shell
fixture, reusing the current source literals in `test_cards_container.py` without
importing pytest/VM fixtures. `builtin-chain` uses current Input ports.
The language probe keeps the unchanged Collatz R/Julia branches and checks that
27 produces 112 values. Its first/rerun records and per-stage times allow SDK or
worker-build comparisons on a fixed daemon/nixpkgs. Both probes keep run data.

## Static validation without execution

These tools need sockets and Nix for measurements. In a restricted sandbox:

```sh
PYTHONPYCACHEPREFIX=/tmp/bench-pycache python3 -m py_compile tests/bench/*.py
python3 tests/run-isolated.py python3 -m pytest -q -m static tests/static
```

With generated bindings and Python dependencies present, also check imports
without opening sockets:

```sh
PYTHONPATH=tests/bench python3 -c 'import measure_stages, measure_language_workers, run'
```

Compilation/imports and static tests do not establish that these measurement
scripts have been run or that a native/remote benchmark succeeds.
