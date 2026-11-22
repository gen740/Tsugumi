# Card ABI 9 cutover lanes

Branch each lane from the shared contracts with `origin/main` merged through
#422. Send changes outside your lane to the file owner. The lead owns
`protocol/**`, `cmake/protocol.cmake`, the Swift FlatBuffers plugin,
`docs/dev/abi9-lanes.md`, `docs/Protocol.md`, and
`tests/core/test_rpc_schema.py`. The lead integrates the lanes.

## Shared contracts

| Boundary | Interface |
| --- | --- |
| Card ↔ worker | [`card_abi.h`](../../protocol/include/tsugumi/card_abi.h) declares version 9 and `tsugumi_main(const tsugumi_call*, tsugumi_bytes*)`. [`card.fbs`](../../protocol/card.fbs) defines `CardInput{inputs:[CardPort], params:[ubyte]}`, `CardPort{name, kind, bytes}`, and `CardResult{outputs:[CardPort], failure:CardFailure}` with `CardFailure{kind, message, source, line, cell}`. Return 0 for a result, including an encoded failure. |
| Worker ↔ core | [`worker.fbs`](../../protocol/worker.fbs) defines `WorkerCall{inputs, params, outputs, deadline_ms}`. Each input carries `FlatBufferData{schema_bfbs, root_type, bytes}`; output declarations are `Data.Port`. `WorkerStart.environment` carries launch secrets. The terminal frame is `WorkerResult{outputs:[WorkerOutput{name, bytes, forwarded_from}]}` or `WorkerFailure{failure:Data.Failure}`. Remote worker contract is 13. |
| Card definition → core → host | Card definitions have no `language` field. `InputTypes` returns resolved ports and FlatBuffers schema, with neither a language selector nor host `DescribeInput`. `FileFormatRequest{path, text}` selects JSON or Nix by extension. `FormatLanguage` is gone. SDK-specific types and code generation belong to the kit. |
| Host ↔ UI and Agent | [`host.fbs`](../../protocol/host.fbs) lists `TemplateEntry{store, name}` via `TemplateListRequest`. `ProjectTemplateCopy` takes the store and name and copies a graph with fresh Card IDs. `TemplateAdd` is a project operation for the versioned `templates/` store. `HostTemplateAddRequest` writes to the host support folder's `templates/` store. Both accept a folder or a public HTTPS Git repository, ref, and subdirectory. |
| Card → worker → core | The ABI effect is `int (*send)(void* host, const char* view, const uint8_t* bytes, size_t size)`. It remains valid for the process lifetime, including after a call returns. The private frame is `WorkerDisplay{view, bytes}` with `call_id = 0`. The worker's relay hands off `WorkerDisplayData{view, bytes}` through `WorkerProc::DisplaySink`. |
| Definition → core → UI | A Card explicitly declares `views = [ { name; type = "dawn"\|"html"; blob; } ]` and can concatenate `sdk.views`. The RPC sends a full `CardViewsUpdated{card, views:[Data.CardView{name, type, blob}]}` through the project subscription. `blob` is the immutable store path to one file. `type` is a string. The list is available before a run, and an empty list clears previous declarations for the Card. |
| Core → UI (view files) | `CardViewReadRequest{card, index, name, offset, limit_bytes}` returns a `FileContent` page (at most 1 MiB, default 1 MiB) of the blob of entry `index` of the Card's current declarations, whose name must be `name`, paged with `next_offset` and `eof` like the other file reads. `name` is the view name and `hash` is the declared store path. The core serves only blobs of the list it last sent in `CardViewsUpdated` (`Service::set_card_views`). Names may repeat, so the core never selects a declaration by name. It rejects an index whose entry has another name with `not_found` and an offset past the end with `invalid_range`. It neither checks nor interprets the bytes. UIs read view files through this request, including when the store is inside the VM. |
| Core → UI | `DisplayUpdated{display:Data.Display{card, run_id, view, bytes}}`. The core stamps the Card and active run; `run_id` is absent between runs. `DisplayRunStateChanged{card, run_id, state}` carries `started` or `ended`. Use [`data.fbs`](../../protocol/data.fbs) and [`tsugumi.fbs`](../../protocol/tsugumi.fbs). The queue keeps the latest pending data per Card and view. Preserve control message order. |
| UI → HTML | In an isolated, network-disabled frame, transfer a MessagePort with `{tsugumi: 1, port}`. Send `{type: "data", bytes: ArrayBuffer}` and `{type: "run", state: "started"\|"ended"}` on that port. Show a placeholder for `dawn` until the native host is available. |

The core relays declarations and data without resolving view names or
interpreting their bytes. Serving a declared blob through `CardViewRead` is
transport: the core reads only the file of the declaration the UI selected. The UI matches the name against the
Card's declarations and hosts the declared type. Unknown and repeated names are handled in
the UI. Display messages and control events are transient, including for
remote runs. The display path has no history rows, output files, trace
payloads, or `ModuleRead` request.

## Exclusive ownership and acceptance

### Nix evaluation

- Own `flake.nix`, `flake.lock`, `nix/**` except the Web files below,
  `CMakeLists.txt`, `cmake/**` except `cmake/protocol.cmake`, `sdk/**`,
  `workerd/cmake/*install*.cmake`, `hostd/cmake/host.cmake`, `pack/**`,
  `tsugumid/runtime/src/{daemon.cc,nix.cc,nix.hh,nix_eval.cc,nix_eval.hh}`,
  `hostd/src/{main.cc,environment_native.cc}`, `hostd/tools/build-macos.sh`,
  `cli/src/launch.cc`, `cli/package-{linux,macos}.sh`,
  `tests/core/doubles/nix_eval.cc`, the three Nix evaluation tests listed
  below, `tests/tools/check_boundaries.py`, `tests/ci/**`,
  `tests/static/{test_ci_selection,test_marimo_bindings}.py`, and SDK-only
  tests outside the docs/demos lane. Coordinate the `nix_dir` field change
  in `tsugumid/runtime/src/service.hh` with the tsugumid lane.
- Keep the host's nixfmt and template-import Git available in the packages.
  Coordinate bundled resources with the Mac UI lane. Keep Card evaluation
  language-independent and use the kit's builders.
- Supply the declared `CardView` fields without name lookup, implicit
  passthru merging, or source inspection. Put the ordered views list, run
  derivations, and the worker when needed in the build plan closure.
  Its JSON has `runs:[store path]`, `views:[{name,type,blob}]`, and the
  optional `worker` executable store path for remote or foreign execution.
  Own the
  shared `workerd/include/tsugumi/workerd/card_plan.hh` used by the worker
  lane. Use the pkgs-taking kit entry and test the vendored ABI files against
  the canonical files before pinning kit.
- Accept when `tests/core/integration/test_definition_evaluation.py` and
  `tests/core/{test_run_lists,test_run_environments}.py` cover derivation and
  labelled-list runs, lazy exports, third-party builder binding, an empty or
  explicit views list, closure contents, and rejected Bash or removed
  attributes. Verify that the foreign guest and remote job use the worker
  from the plan and that the kit checks pass on macOS and Linux.

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
  and `WorkerStart` for secrets. Use the binary parameter payload for local
  and remote workers.
- Accept when a v9 Card runs and v8 and invalid results fail cleanly,
  an encoded failure retains source position, and an unknown stale call ID
  leaves the worker serving. Exercise cancellation from Card threads,
  opaque post-return data, and the Linux and macOS output boundary.
  Check forwarding, snapshots, and the default 64 GiB and 1,000-entry
  output budgets through both local and real remote execution.

### tsugumid

- Own `tsugumid/document/**`, all other files in `tsugumid/runtime/**`,
  `hostd/src/{service,templates,project_operations,agent_exclude,input_types,format}.cc`,
  the corresponding private headers, and `hostd/agent/**`. Own the project
  subscription, history, run, remote-recovery, and Host and Agent tests and
  fixtures under `tests/core/**`, `tests/fixtures/service-contract.json`,
  and `tests/static/test_generated_shared.py`.
  Worker, Nix evaluation, and shared-contract files named above stay with
  their owners.
- Preserve host ownership of template listing, copying, and Git import.
  Project stores participate in project versioning. Agent `template_list`
  and `template_copy` respect project access, and `template_add` and
  `template_import` require approval.
- Consume the Nix view list and `DisplaySink`. Publish
  `CardViewsUpdated` on visibility and definition changes, and pass the same
  list to `Service::set_card_views` so `CardViewRead` serves it (an empty list
  clears it). Stamp `DisplayUpdated` using the active run at arrival, and
  publish `DisplayRunStateChanged` around each Card's participation.
  Coalesce only pending data for the same Card and view within a control
  interval.
- Accept when RPC tests show the declaration list before any run, deliver
  arbitrary bytes including NUL and non-UTF-8 under the original view name,
  cover start/data/end order and slow subscribers, and verify post-return
  delivery including with no active run. Check that history and tracing keep
  no display data and that no remote result file contains display data.

### Web UI

- Own `ui/web/**`, `ui/shared/modules/**`,
  `ui/shared/i18n/en.json`, and `nix/{web.nix,web-checks.nix}`.
  Coordinate shared viewer or localization edits requested by the Mac lane.
  Keep the Web template picker and its tests on the Host RPCs.
- Consume generated TypeScript `CardViewsUpdated`, `DisplayUpdated`,
  and `DisplayRunStateChanged`. Read declared HTML files with
  `CardViewReadRequest` in pages until `eof`, and host them in a sandboxed
  iframe in the browser. Do not read view blobs from the store directly.
- Accept when `web-typecheck` and the Web behavior tests cover two
  independently addressed views, a pre-run empty frame, byte-for-byte
  MessagePort delivery, ordered control messages, network isolation, frame
  disposal, the Dawn placeholder, and a distribution without bundled views.

### Mac UI

- Own `ui/macos/**` and `client/swift/**` except the lead's FlatBuffers
  plugin. Coordinate via the Web owner for shared viewer and strings files.
  Keep `TemplatePicker.swift` and `SessionEditing.swift` on the Host RPCs.
  Coordinate the bundled Git and nixfmt resources with the Nix evaluation
  lane.
- Consume the same three notifications in the generated Swift types.
  Load the declared HTML with `CardViewReadRequest`, paging until `eof`,
  because the store is inside the VM. Keep it inside an isolated WebKit frame. Use the same MessagePort contract as Web.
- Accept when Swift tests and a signed background app run verify pre-run
  views, per-name routing, start/data/end, post-return bytes, network
  isolation, teardown, and the Dawn placeholder. Check the app bundle for
  bundled view code. Use `ui/macos/tools/run-ui.sh` for the app run.

### Docs/demos

- Own `docs/**` except this lane file and the generated `docs/Protocol.md`,
  `demos/**`, `tests/python/test_demo*.py`, and `tests/fixtures/card-templates/**`.
  Coordinate the demo player with Web when shared viewer behavior changes.
- Use language-free Card definitions. Document the Project and Host template
  stores. The examples ship in the kit, and `tests/fixtures/card-templates/`
  holds test inputs formerly under `ui/shared/templates/`. Coordinate any
  fixture changes with the Nix evaluation lane's SDK tests and the tsugumid
  lane's generated-file check.
- Document and use the explicit `views` list, ABI 9 `CardInput` and
  `CardResult`, and the kit's `card::display::send(...)` interface. Port
  both Japanese and English demos for all eight SDKs, including Drafts.
- Accept when the demo suite builds and runs on its supported platforms, the
  Japanese and English trees match, and the docs site and demo player build
  and pass their tests. Publish live-view examples without treating display
  traffic as a retained scientific result.

Run the static and lane-specific tests during development. Use the repository's
`tests/check-fast.sh` before push and the provided `heavy.sh` for heavy builds.
