# Card ABI 9 cutover lanes

Branch each lane from the shared-contract commit. File ownership starts there.
Send changes outside your lane to the file owner. The lead owns
`protocol/**`, `cmake/protocol.cmake`, the Swift FlatBuffers plugin,
`docs/Protocol.md`, and `tests/core/test_rpc_schema.py`. The lead integrates
the lanes and merges `origin/main` after #422 lands.

## Shared contracts

| Boundary | Interface |
| --- | --- |
| Card ↔ worker | [`card_abi.h`](../../protocol/include/tsugumi/card_abi.h) declares version 9 and `tsugumi_main(const tsugumi_call*, tsugumi_bytes*)`. [`card.fbs`](../../protocol/card.fbs) defines `CardInput{inputs:[CardPort], params:[ubyte]}`, `CardPort{name, kind, bytes}`, and `CardResult{outputs:[CardPort], failure:CardFailure}` with `CardFailure{kind, message, source, line, cell}`. Return 0 for a result, including an encoded failure. |
| Worker ↔ core | [`worker.fbs`](../../protocol/worker.fbs) defines `WorkerCall{inputs, params, outputs, deadline_ms}`. Each input carries `FlatBufferData{schema_bfbs, root_type, bytes}`; output declarations are `Data.Port`. `WorkerStart.environment` carries launch secrets. The terminal frame is `WorkerResult{outputs:[WorkerOutput{name, bytes, forwarded_from}]}` or `WorkerFailure{failure:Data.Failure}`. Remote worker contract is 13. |
| Card → worker → core | The ABI effect is `int (*send)(void* host, const char* view, const uint8_t* bytes, size_t size)`. It remains valid for the process lifetime, including after a call returns. The private frame is `WorkerDisplay{view, bytes}` with `call_id = 0`. The worker's relay hands off `WorkerDisplayData{view, bytes}` through `WorkerProc::DisplaySink`. |
| Definition → core → UI | A Card explicitly declares `views = [ { name; type = "dawn"\|"html"; blob; } ]` and can concatenate `sdk.views`. The RPC sends a full `CardViewsUpdated{card, views:[Data.CardView{name, type, blob}]}` through the project subscription. `blob` is the immutable store path to one file. `type` is a string. The list is available before a run, and an empty list clears previous declarations for the Card. |
| Core → UI | `DisplayUpdated{display:Data.Display{card, run_id, view, bytes}}`. The core stamps the Card and active run; `run_id` is absent between runs. `DisplayRunStateChanged{card, run_id, state}` carries `started` or `ended`. Use [`data.fbs`](../../protocol/data.fbs) and [`tsugumi.fbs`](../../protocol/tsugumi.fbs). The queue keeps the latest pending data per Card and view. Preserve control message order. |
| UI → HTML | In an isolated, network-disabled frame, transfer a MessagePort with `{tsugumi: 1, port}`. Send `{type: "data", bytes: ArrayBuffer}` and `{type: "run", state: "started"\|"ended"}` on that port. Show a placeholder for `dawn` until the native host is available. |

The core relays declarations and data without resolving view names, reading
blob contents, or interpreting their bytes. The UI matches the name against the
Card's declarations and hosts the declared type. Unknown names are handled in
the UI. Display messages and control events are transient, including for
remote runs. The display path has no history rows, output files, trace
payloads, or `ModuleRead` request.

## Exclusive ownership and acceptance

### Nix evaluation

- Own `flake.nix`, `flake.lock`, `nix/**` except the Web files below,
  `CMakeLists.txt`, `cmake/**` except `cmake/protocol.cmake`, `sdk/**`,
  `workerd/cmake/*install*.cmake`, `pack/**`,
  `tsugumid/runtime/src/{daemon.cc,nix.cc,nix.hh,nix_eval.cc,nix_eval.hh}`,
  `hostd/src/{main.cc,environment_native.cc}`, `hostd/tools/build-macos.sh`,
  `cli/src/launch.cc`, `tests/core/doubles/nix_eval.cc`, the three Nix
  evaluation tests listed below, `tests/tools/check_boundaries.py`,
  `tests/ci/**`, `tests/static/{test_ci_selection,test_marimo_bindings}.py`,
  and SDK-only tests outside the docs/demos lane.
  Request the `nix_dir` field change from the owner of `service.hh`.
- Supply the declared `CardView` fields without name lookup, implicit
  passthru merging, or source inspection. Put the ordered views list, run
  derivations, and the worker when needed in the build plan closure.
  Its JSON has `runs:[store path]`, `views:[{name,type,blob}]`, and the
  optional `worker` executable store path for remote or foreign execution.
  Own the
  shared `workerd/include/tsugumi/workerd/card_plan.hh` used by the worker
  lane. Use the pkgs-taking kit entry and test the vendored ABI files against
  the canonical files before pinning kit.
- Accept when `test_definition_evaluation.py`, `test_run_lists.py`, and
  `test_run_environments.py` cover derivation and labelled-list runs, lazy
  exports, third-party builder binding, an empty or explicit views list,
  closure contents, and rejected Bash or removed attributes. Verify that the
  foreign guest and remote job use the worker from the plan and that the kit
  checks pass on macOS and Linux.

### Worker

- Own `workerd/**` except the install CMake files and `card_plan.hh`,
  `vm/guest/src/vmd_main.cc`, `tests/remote/**`, the worker double,
  `tests/core/{native_contract_test.cc,local_worker_test.cc,worker_staging_test.cc,worker_execution_test.cc,test_worker_loading.py}`,
  and the integration tests for worker loading, resident workers, staging,
  invocation isolation, and Card secrets.
- Consume `card.fbs`, `card_abi.h`, and `worker.fbs`. Implement the
  process-lifetime reader and hand the opaque frame to `DisplaySink`.
  Coordinate remote streaming and sink lifetime with tsugumid. Deliver
  post-return data through the live channel. Use launch-fixed directories
  and `WorkerStart` for secrets.
- Accept when a v9 Card runs and v8 and invalid results fail cleanly,
  an encoded failure retains source position, and an unknown stale call ID
  leaves the worker serving. Exercise cancellation from Card threads,
  opaque post-return data, and the Linux and macOS output boundary.
  Check forwarding, snapshots, and the default 64 GiB and 1,000-entry
  output budgets through both local and real remote execution.

### tsugumid

- Own all other files in `tsugumid/runtime/**`, `hostd/src/service.cc`,
  `hostd/agent/**`, and the project subscription, history, run, and
  remote-recovery tests and fixtures under `tests/core/**` and
  `tests/fixtures/service-contract.json`. Worker and Nix test files named in
  their lanes stay with those owners.
- Consume the Nix view list and `DisplaySink`. Publish
  `CardViewsUpdated` on visibility and definition changes, stamp
  `DisplayUpdated` using the active run at arrival, and publish
  `DisplayRunStateChanged` around each Card's participation. Coalesce only
  pending data for the same Card and view within a control interval.
- Accept when RPC tests show the declaration list before any run, deliver
  arbitrary bytes including NUL and non-UTF-8 under the original view name,
  cover start/data/end order and slow subscribers, and verify post-return
  delivery including with no active run. Check that history and tracing keep
  no display data and that no remote result file contains display data.

### Web UI

- Own `ui/web/**`, `ui/linux/**`, `ui/shared/modules/**`,
  `ui/shared/i18n/en.json`, and `nix/{web.nix,web-checks.nix,linux-ui-check.nix}`.
  Coordinate shared
  viewer or localization edits requested by the Mac lane.
- Consume generated TypeScript `CardViewsUpdated`, `DisplayUpdated`,
  and `DisplayRunStateChanged`. The Web host reads declared HTML files
  through the platform store access path and the browser hosts them in a
  sandboxed iframe. Scope file access to the declared immutable view blobs.
- Accept when `web-typecheck` and the Web behavior tests cover two
  independently addressed views, a pre-run empty frame, byte-for-byte
  MessagePort delivery, ordered control messages, network isolation, frame
  disposal, the Dawn placeholder, and a distribution without bundled views.

### Mac UI

- Own `ui/macos/**` and `client/swift/**` except the lead's FlatBuffers
  plugin. Coordinate via the Web owner for shared viewer and strings files.
- Consume the same three notifications in the generated Swift types.
  Load the declared HTML through the platform store access path and keep it
  inside an isolated WebKit frame. Use the same MessagePort contract as Web.
- Accept when Swift tests and a signed background app run verify pre-run
  views, per-name routing, start/data/end, post-return bytes, network
  isolation, teardown, and the Dawn placeholder. Check the app bundle for
  bundled view code. Use `ui/macos/tools/run-ui.sh` for the app run.

### Docs/demos

- Own `docs/**` except this lane file and the generated `docs/Protocol.md`,
  `demos/**`, `tests/python/test_demo*.py`, and `tests/python/test_demos.py`.
  Coordinate the demo player with Web when shared viewer behavior changes.
- Document and use the explicit `views` list, ABI 9 `CardInput` and
  `CardResult`, and the kit's `card::display::send(...)` interface. Port
  both Japanese and English demos for all eight SDKs, including Drafts.
- Accept when the demo suite builds and runs on its supported platforms, the
  Japanese and English trees match, and the docs site and demo player build
  and pass their tests. Publish live-view examples without treating display
  traffic as a retained scientific result.

Run the static and lane-specific tests during development. Use the repository's
`tests/check-fast.sh` before push and the provided `heavy.sh` for heavy builds.
