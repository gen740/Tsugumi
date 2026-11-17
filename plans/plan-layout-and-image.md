# Program boundaries and release-built guest — #290 + #293

## Basis, constraints, and settled choices

| Item | Plan basis |
| --- | --- |
| Inspected checkout | `claude/layout`, HEAD = `origin/main` = `abb6f15b6065e47c83144d43eb34694fc627d45a` (v0.9.0); initially clean. |
| Inputs read | `AGENTS.md`, `docs/Philosophy.md`, supplied `issue-290.md`, `issue-293.md`, `plan-layout-move-old.md`, `git log v0.8.1..origin/main`, and implementation/build/packaging sources named below. No builds or tests executed. |
| Delivery | One branch, one PR; A: splits at existing paths and #293; B: pure move then paths only; C: dependency enforcement. This document is the only written file, outside the repository. |
| Fixed layout | `ui/{shared,web,macos}`, `vm/{macos,guest}`, `util`, `rpc`, `protocol`, `tsugumid/document`, `hostd/agent`, `workerd`. Reserve future UI Linux/Windows and VM Linux names in documentation; create no empty implementation roots. |
| Fixed products | Keep executable names, `TSUGUMI_*` names unless their feature is deleted, user support locations, project formats, SDK ABI, installed Web/CLI paths, and app update URLs. No old-path shims, migrations, aliases, or dual boot implementations. |
| Guest choice | One versioned image payload bundled by the app, rebuilt with Tsugumi; app updates download the whole app. Keep Rosetta / explicitly requested qemu / none behavior unchanged. |
| Philosophy | Building/reusing a definition-fixed system image is allowed. Do not change run-result retention/reuse, reset consent, or automatic deletion policy. Store integrity repair and orderly stop already exist; preserve them. |
| Documentation | `docs/Development.md` is absent. Use actual code and existing architecture docs; do not invent its missing sections. Documents, comments, and commit messages are English. |

**One unavoidable scheduling exception:** a pure `git mv` commit cannot also leave this tree buildable: root CMake calls `add_subdirectory(core)`, Nix uses literal filesets, and Swift/scripts resolve `macOS` and `web`. B1 must therefore be an inspection-only intermediate commit immediately followed by B2; their pair is the buildable checkpoint. No compatibility links or path autodetection will be introduced to hide this. If literally every commit must build, squash B1+B2 into one commit and retain the pure-move diff as an external review artifact. Owner choice D5 below; all A/C commits and B2 must build independently. Neither B1 alone nor any incomplete phase is mergeable.

## Current-main corrections to the old plan

| Old assumption | Current implementation and consequence |
| --- | --- |
| S12 still needs a client-save cutover | Done: `2ab0a865`, `90a4d694`, `7998ca74`. Mac `ProjectSession`/`SessionEditing` and Web session submit Host operations; guards and Undo use the operation queue. Remove S12 work, retain regression coverage. Host save/document-edit internals are still needed. |
| Local core owns worker slots; one-path work follows migration | `c5e24d6c`: `LocalExecutionService` calls `LocalWorkerClient`; workerd owns pool slots through `local_service.cc`, inherited socket and descriptor handoff. Preserve EOF/lifetime rules, reuse, local direct descriptors and remote relay. Do not schedule another ownership cutover. |
| Worker implementation no longer reaches core | Still false: `workerd/CMakeLists.txt` links `tsugumi_core`/`tsugumi_nix_eval`; runtime objects include private core headers and are embedded in core. Split the link/type dependencies without redesigning execution. |
| Three layout units are compiled into core | Already fixed. Host target owns them; move their `-ffp-contract=off` source property with host ownership. |
| Agent source/tool visibility has no additional owner | `host/agent_exclude.*`, `agent_policy.hh`, `demo_references.*`, `file_miss.*`, operation authorization and shell staging are host policy. Preserve explicit exclusion semantics, recursive linked externals, file-removal refusal and human/Agent distinction. |
| Atomic edits need new generic services | `host/{project_operations,project_edit,document_edits,save,layout_saving}.*` already implement them. They move to hostd; do not push semantic transformations back into document or clients. |
| Guest lifecycle is just starting/stopping a process | vmd, VMHost, Host process adapters and app lifetime now implement ordered shutdown, Nix-daemon termination, disk sync, marker clearing, dirty/image-change store checks and repair. Include `store-health.sh`, `docs/VMStoreHealth.md`, `vm_lifetime_test.cc` and app quit tests in the move. |
| Boot disk is in another repository | `923b0b85`: `publish-vm.sh` now publishes `vm-<hash>` releases in **this** repository. #293 replaces that current path too; do not propose merely switching repositories. |
| Web checks compile an ad hoc host edit implementation | `nix/flake/checks.nix` now builds `tsugumi_document_edit_driver` from installed production archives; `nix/web-checks.nix` consumes it. Preserve this reuse. |
| Old static test/source inventory | `test_card_identity.py` is gone; identity coverage is native. Add first-start/script-launch scans and current local-worker schema; do not resurrect removed tests or obsolete MCP generator outputs. |

## Assumptions before mechanisms or protections

These are the reasons for the changes below, not additional product features.

| Assumption / evidence | Actual problem | Realistic? / what the measure achieves |
| --- | --- | --- |
| Independent programs must compile without one another's private headers; core/JSON/document/TLS currently export `core/src` and workerd links the core archive. | Directory names alone cannot enforce #290. | Present CMake dependencies, not a hypothetical misuse. Narrow targets plus include/link checks catch structural reintroduction; review still decides semantic ownership. |
| Transport must be reusable; `server.hh` exposes Project `Note`, visibility and generated request types, while client transports include `client.hh`. | A moved server would still know Cards and Agent messages. | Present code. Endpoint adapters remove protocol knowledge; ordered-loop tests detect sequencing regressions but do not prove all concurrency behavior. |
| A development build must use the image for its current definition; build-app/CLI currently accept any existing vmlinux/rootfs pair. | New Tsugumi sources could silently run an older guest. | Existing existence-only checks become incorrect once code enters the image. Compare the evaluated full image identity and checksums before reuse; this detects mismatch/corruption, not malicious CI or all runtime bugs. |
| Boot compilation and manual publication have failed (issue #293); `/init` still hashes `/source` and runs `nix build`. | Missing shipped source, untested guest binary, manual publish dependency. | Reported failures plus current code. Build once on Linux CI, consume the same artifact, delete source-on-boot. Card definitions may still build normally. |
| Shipping the populated closure expands redistribution; inventory currently assumes nixpkgs packages and boot-only own scripts. | Tsugumi/hosq/local sources and additional libraries would be missing or misclassified. | Concrete `OWN`, nixpkgs attribute lookup, fixed-output-only source collection and `BUILD_FILES` limitations. Full closure inventory/source reconstruction addresses completeness; it is not a claim of legal certification. |
| VZ uses separate kernel and block-device URLs (`SharedVM.configuration`). | A literal raw single disk cannot replace both inputs without a new boot architecture. | Present API calls. One distributable archive contains kernel+rootfs; helper extraction supplies both URLs. No split base/Tsugumi image or custom disk format. |
| Pure renames change repository depth and source-derived Nix identities. | Filename grep alone misses ancestor walks, filtered sources, installed roots and zero-match scans. | Present constructors and filesets listed below. Inventory/build/resource checks catch these specific failures; no promise of bit-identical signed binaries. |

## Destination ledger

Recursive rows preserve suffixes; more specific rows and the split ledger override them. Each tracked file must receive exactly one destination or an explicit deletion in A. Use this rule to generate a complete file map at A's final parent, rather than reusing the old directory counts.

| Current path / family | Final owner / destination |
| --- | --- |
| `core/` | Remove root after the allocations below; no catch-all public core source include. |
| `core/include/tsugumi/core/daemon.hh` | `tsugumid/include/tsugumi/core/daemon.hh`. |
| `core/src/{document,graph,types,nix_refs,collect_type,host_requirements,input_types,worker_condition}.{cc,hh}` | `tsugumid/document/{src,include/tsugumi/document}/`; definition resources/path/type contracts belong here, with no model/history/lifecycle imports. `input_types` resolves graph ends; Host rendering remains in hostd. |
| `core/src/worker_selection.hh` | `tsugumid/src/`; live candidate/free/resident selection is scheduling, unlike the document's worker-condition expression. |
| `core/src/util/{crypto,fs,log,process,sqlite,text}.{cc,hh}`, `sparse.hh` | `util/{src,include/tsugumi/util}/`; extract scratch-root policy first. Ordinary handles are mechanisms, not permission for a registry/cache service. |
| `core/src/schema.{hh,cc}` | S2: generic JSON → `util/json/`; project schemas → document; worker manifests → `workerd/`; persisted execution/project/startup schemas → tsugumid. |
| `core/src/server.{hh,cc}`, `lanes.*`, client transport implementations | S1: generic server/dispatch/native/Wasm transport → `rpc/`; domain endpoint adapters → tsugumid or hostd/agent. |
| `core/src/util/peer_tls.*` | TLS/identity/socket mechanisms → `rpc/tls/`; authorized-file policy → `workerd/`; pinned-peer decisions remain hostd. |
| `core/src/{model,wire}.*` | S3: definition types → document; invocation values/failures/display codecs → workerd; run/history/project replies → tsugumid. |
| `core/src/module_source.*` | S5: assembly/identity/limits → `workerd/`; project module cache → `tsugumid/`. |
| `core/src/{worker_derivation.hh,derivation_transport.cc}` | `workerd/nix/` public transfer contracts and implementation. |
| `core/src/nix_eval.*` | Evaluator/reuse → tsugumid; process/store/substituter/rooted-usage mechanisms → `workerd/nix/` narrow target, C++23 private. |
| `core/src/{cache_usage.*,run_scratch.*}` | `tsugumid/runtime_support/` for shared accounting/measurement (D3); private project policy stays tsugumid. Export only the subset vmd actually needs. |
| `core/src/vmd_main.cc` | `vm/guest/src/vmd_main.cc`; lifecycle, per-project users/mounts, store checks and supervisor behavior remain guest-owned. |
| `core/src/host/**` | `hostd/{src,include/tsugumi/host}/**`: includes operations, exclusion, settings, credentials, layout, requirements rendering, environments/broker/VM process adapters. Private headers stay private. |
| `core/src/agent/**` + `agent/Shared/**` | `hostd/agent/{src,include,Shared}/`; generated Swift adapter still belongs to UI. Agent wire contracts stay `protocol/`. Remove top-level `agent/`. |
| All other direct `core/src/*` | `tsugumid/src/`: daemon/main, service/run/shell, scheduler, Nix orchestration, execution services/remote job/invocation, store/history, collect_value, versions/private version records, permissions/log policy and their headers. Explicit exceptions above take priority. |
| `core/CMakeLists.txt`, root CMake target blocks | A prepares fragments at existing paths; B moves them to each owner's `CMakeLists.txt`. No target/source-list redesign hidden in B2. |
| `client/**` | Keep domain-aware facade/C/Wasm/CLI APIs; only generic transport implementation/interface moves to rpc. Product names stay stable. |
| `workerd/**` | Keep plus worker values/codecs/module/store support; add existing `local_{client,service,transport}.*` to explicit manifests. Pool/lifetime ownership stays workerd. |
| `sdk/**`, `sdk/describe/**` | Keep ABI, runtimes, generators, views and input renderers; rewrite installed worker staging to narrow extracted JSON/text sources. |
| `ui/{modules,templates}/**` | `ui/shared/{modules,templates}/**`; shared templates remain resources embedded by hostd. |
| `web/**` | `ui/web/**`; server/browser/shared TS source seams below. Keep installed `share/tsugumi/web/dist` and CLI `web/dist`. |
| `macOS/Sources/App/**`, Grammars, UI resources/tests/tools | `ui/macos/**`; split common policy first. Preserve syntax resources, localizations, bundles, help, player data and assets. |
| `macOS/Sources/AgentClient/**` | UI/RPC/observation/DTO adapters → `ui/macos/Sources/AgentClient`; VM image/resources/config DTOs → VM-owned Foundation target; credential policy → hostd. |
| `macOS/Sources/Launcher/{SharedVM,ExecutionBrokerBridge,VMHostIPC,VMReset}.swift` | `vm/macos/Sources/VMRuntime/`; extract cache policy and AgentClient dependency. Preserve shutdown and supplied reset retention. |
| Launcher `{EnvironmentManager,TerminalPipe,AppChannel,LauncherError}.swift`, UI parts of `Environment.swift` | `ui/macos/Sources/Launcher/`; shared wire/error/value types only in a small non-UI target. HostLock → VM runtime; ProjectIdentity remains an explicitly nonauthoritative UI hint. |
| `Launcher/BinaryCaches.swift`, policy parts of Settings and Provider | S7/S9 → hostd; UI edits DTOs, platform primitive follows D1. No second provider/source-precedence implementation. |
| `macOS/Sources/{VMHost,VMCtl}/**` | `vm/macos/Sources/{VMHost,VMCtl}/**`, independent Swift package and tests; same executable product names. |
| `macOS/Sources/{RPC,ClientC}/**`, `Plugins/FlatcPlugin` | Recommended D4: `client/swift/` minimal package for UI and VM; includes FlatcPlugin once, without AgentClient/renderer dependencies. This is protocol-aware client adaptation, keeping rpc strictly transport-only. |
| `macOS/Sources/KeychainUI/**` | Minimal OS primitive under `hostd/platform/macos/`; process placement D1. Do not move SwiftUI or conversation DTOs with it. |
| `macOS/Container/**` | `vm/guest/**`: kernel/config/provenance, boot/system Nix, store-health, versions, local image builder; delete vm-source.txt in A. |
| `macOS/tools/{vm-inventory.py,vm-source-bundle.py}` | `vm/guest/tools/`; full-image inventory/source, independent of app assembly. Delete publish-vm.sh in A. |
| `macOS/tools/build-host.sh` | `hostd/tools/build-macos.sh`; retain pinned native formatter/dylib packaging integration. |
| `macOS/tools/build-vmctl.sh` | `vm/macos/tools/build-vmctl.sh`; independent VM package, signing and entitlements. |
| `macOS/tools/release.sh` | `cli/release.sh`; repository version PR/tag helper. |
| `macOS/build-app.sh`, remaining tools and resources | `ui/macos/`; VM helper entitlement input → `vm/macos/Resources/`; preserve actual entitlements per signed product. |
| `macOS/CoreTests/**` | UI adapters stay `ui/macos/CoreTests`; `ExecutionBrokerBridgeTests`, `VMResetTests` → `vm/macos/Tests`; BinaryCache/SecretStore policy cases → host tests, retain OS/UI adapter cases. Include current `DocumentRevisionsTests` in UI operation coverage. |
| `macOS/{Package.swift,Package.resolved,CoreTests/Package*}` | UI package/CoreTests at `ui/macos`; VM/RPC manifests prepared in A and moved to respective roots. Keep dependency pins unchanged. |
| `protocol/**` | Keep all schemas including `local_worker.fbs`, `vm_host.fbs`, host operations, service common; generated outputs stay build outputs. |
| Support roots | Keep `.agents`, `.claude-plugin`, `.github`, `cli`, `cmake`, `demos` (both languages and every nested external), `docs`, `nix`, `plugins`, `tests`, `third-party`. Only references change. |
| Root files | Keep root build/flake/lock/VERSION/licenses/notices/README/RELEASING/AGENTS/ignore/attributes. Preserve `CLAUDE.md -> AGENTS.md`; no generated artifacts or worktree metadata moves. |

## Refreshed split ledger (A, before bulk relocation)

The following preserves the old S identifiers for review. S12 is closed and omitted from implementation assignments.

| ID | Current seam, including new consumers | Required result / focused evidence |
| --- | --- | --- |
| S1 | `server.*`, `lanes.*`, `client.hh`, native/Wasm transports, project Service and Host/Agent services | Protocol-free bytes/connection IDs/cancellation/coalescing in rpc. Move generated verification, Hello/failure/reply inspection, notification envelopes, Card visibility and subscription sequence stamping into owner endpoint adapters. Apply endpoint subscription/visibility/send operations on the same ordered loop; preserve Open reply → initial display → patches. Client facade passes opaque subprotocol to transport. Check disjoint-visible peers, slow outboxes, cancellation and operation ordering. |
| S2 | `schema.*`, `agent/json.hh`, worker display/native/journal/invocation, project store/Nix | Extract generic aliases/strict encode/decode/UTF-8 diagnostics into JSON utility. Project/graph/Card definition schemas → document; ManifestRun/CardManifest → workerd contract; project JournalRecord/View/history/saved inputs/build cache/pins/SDK job/ready records → owning project runtime. Do not mistake workerd's own journal implementation for the project store's JournalRecord. Keep exact field/validation semantics. |
| S3 | `model.*`, `graph.hh`, `wire.*`, workerd public worker types and local service/client | ExecutionResources/path/port definition contracts stop importing run/history models. Worker failure/value/display/invocation codec target serves both execution paths; project summaries/history/replies stay private. Remove workerd's core archive link and core/src include. Local slots and descriptor protocol stay intact; no new runner design. |
| S4 | `util/peer_tls.*`, workerd remote listener and host execution broker | Generic TLS/socket/key identity in rpc; authorized-file reader/policy in workerd; generic callback for key acceptance. Host retains pinned-peer decisions. Remove broad source-root exports and document's link to worker lifecycle/support. |
| S5 | `module_source.*`, workerd invocation, core service/store | Worker module assembly/identity/size rules separate from immutable project-cache lookup/save. Same bytes and limits; no run-output cache. |
| S6 | `nix_eval.*`, derivation transport, nix_store, cache_usage/run_scratch, vmd | Extract initialization/substituters/store closure usage and transfer from evaluator. Vmd links narrow support, not tsugumi_core/evaluator. Sampler needs both rooted_usage and scratch measurement, not merely a moved header. Preserve Nix validity checks, budgets, progress and evaluator test-double selection. |
| S7 | BinaryCaches.swift, SharedVM preparation, BinaryCacheSettings, host settings/environment | Host owns validation/storage/trust and next-boot private nix.conf/netrc snapshot; RPC edits/status; helper mounts a host-supplied location. Preserve invalid-settings default-only behavior and failure if fresh snapshot cannot be written; never mount stale credentials. No new cache service. |
| S8 | Environment.swift, HostRuntimeConfiguration.swift, LauncherError/AppChannel, SharedVM | VM image/archive/install/config values and locks without AgentClient dependency; UI readiness/channel values remain adapters. Shutdown ownership is already implemented; split imports without duplicating state machines. Remove source fields in #293, not a new generic source DTO. |
| S9 | Provider.swift, CoreHostClient, ApplicationSettings, web providerKeys/keychain, host Settings/SecretStore | Host already selects env → keychain → memory and storage and validates ProviderSet; reuse that policy. Move remaining reusable validation/save decisions from Swift/Web. Keep OAuth refresh/revoke in existing host Credentials. Isolate minimal Keychain primitive (D1), asking/nonasking behavior and test denial flags. No new credential daemon or silent persistence fallback. |
| S10 | host/version_text.*, host/versions.*, versions/version_record, protocol/tsugumi.fbs | RPC already carries private records as JSON; graph/notes already go through project RPC. Replace record_json/private message decoding with typed neutral facts at the existing graph/note seam. Core parses; expose typed commit kind/target, run whole/propagate/saved-input flags and per-Card counts/failure kinds, and changes added/removed/changed-kinds/workflow facts; retain arbitrary user-authored commit text as text. Host localizes/produces model summaries and owns notes/diffs. Delete raw record_json wire fields once consumers use facts, with no dual-format path. Remove version_record include from host. Preserve current stopped-environment semantics; stopped inspection is not a new feature in this PR. |
| S11 | Root/core/workerd/client CMake, installed test support, Swift products/manifests/signing, TS projects | Prepare component manifests/include trees and independent Swift VM/RPC products at existing paths. No copied sources compiled into multiple archives, no testSupport back door. Split TS shared contracts/catalog/localized presentation from browser model and server Node code. Keep target/product/install names through B. |
| Extra: scratch | util/fs run_scratch_root; host/environment.cc:449 and run_scratch.cc | Default policy → tsugumid. Host passes explicit configured root; when no override, let the project daemon choose its default rather than calling project runtime from host. If status needs the effective path, return it over runtime RPC. Preserve existing per-project path/override and retention semantics. |
| Extra: Agent/operations | host/agent_exclude, agent_policy, project_operations/project_edit/document_edits/save, tools_edit/tools_files | No new split of their behavior: keep host-owned authorization/edit/exclusion together; only replace JSON/document/transport imports. Keep demo copy references and exclusions on every tool/shell/mutation path. Existing client queue/Undo remains frontend adaptation. |
| #293 | Guest Nix/init, full closure inventory/source, release/CI, all source/image consumers | One end-to-end A commit after shared interfaces settle: prebuilt closure and new packaging are activated together, all old source/compile/publish paths deleted. Detailed contract below. |

## #293 image, development, and release contract

### Nix and first boot

| Surface | Change at existing paths in A; final path follows ledger |
| --- | --- |
| `nix/flake/packages.nix` | Define vm-system once, pass it into vm-boot. `vm-system` remains the supervisor/tini/project-tools composition, `vm-boot` the kernel/EROFS builder, `vm` the single distributable image archive plus identity metadata. Preserve these flake attribute names. No Apple container in Linux CI. |
| vm-system.nix | Existing supervisor already references `${tsugumid}/bin/tsugumi-vmd`, tsugumid, tini/bash/project PATH/certificates. Keep it; update comments and ensure core output includes worker, workerd, SDK and runtime tools. Do not assume merely adding a worker to PATH roots the sibling binary: include their owning output/closure explicitly. |
| vm-boot.nix | Add vm-system to runtime/closureInfo and directly reference its tini/supervisor from /init. Copy/register every runtime path into lower EROFS; preserve kernel separately inside the archive. Do not include compilers/build-time closure just to boot Tsugumi. Card builds still fetch their own definition tools. |
| vm output | Recommend `tsugumi-vm.tar.gz` containing `vmlinux`, `rootfs.erofs`, and `vm-version` (the full vm-boot output basename, now including vm-system transitively). Derive expected identity by Nix evaluation, never a handwritten source list. Deterministic archive metadata; archive SHA256 in external build manifest. Inventory also records this boot identity. |
| First boot | Format/mount store/history disks as already implemented; overlay lower image store + persistent upper; load full registration, root shipped paths; mount cache snapshot; start Nix daemon/network; run existing required store check/repair; exec prebuilt tini/supervisor. Vmd starts project tsugumid when requested. No source share, source hashing, persistent source-hash system link, flake evaluation or Tsugumi build. |
| Shutdown/store health | Keep dirty marker before writes, clean marker after ordered process/daemon stop and synced writes, image-change verification, repair failure behavior and explicit Reset retention. Full /store-paths hash now reflects Tsugumi updates too. Test A→B image update with the same store/history disks; do not reset them as an update mechanism. |
| x86 | Leave unsafeDiscardStringContext/on-demand qemu-store realization and tsugumi.x86 modes as they are. Do not pull qemu-user into default image closure or promise fully offline first boot with explicit qemu. Rosetta is never installed by the app. |
| Single file consumption | App/CLI carry the archive once. VM helper extracts kernel/rootfs into its existing private support image area before creating VZ attachments; it does not retain an extra bundled expanded copy. Reuse extracted files only for matching image identity and complete contents. This preserves the existing protection against app replacement removing a running VM's files. |

### CI, release, and development consumption

| Stage | Concrete work and dependency |
| --- | --- |
| Resolve release revision | Refactor release.yml's existing version/tag validation into a first job returning immutable commit SHA, tag, version. Dispatch validates VERSION and tags once; tag trigger resolves that tag. Both Linux and Mac checkout that exact SHA. Preserve RC/latest behavior and VERSION-through-PR policy. |
| Build image | Reusable image workflow/job on existing `ubuntu-24.04-arm` / aarch64-linux Nix setup. Build `.#packages.aarch64-linux.vm` and the production native/test packages at the same checkout. Cache kernel and dependencies via existing Cachix mechanisms; PR builds require no write token. Expand current ci-platform `vm` job (today only vm-system) to exercise final image assembly. |
| CI artifact | Upload image, manifest/checksums, license inventory/archive and corresponding sources as one artifact set, named with evaluated guest identity, associated with immutable revision. A manifest carries identity, system, revision and filenames/checksums; it is not a replacement source list. No public vm-hash release used as artifact cache. Add image job to selected prerequisites/result gate; failed inventory/build prevents Mac packaging/publication. |
| Native checks | Test binaries linked to the same Nix production archives as the image; do not rebuild a separate guest executable in the Mac job. Image closure membership/init checks run on Linux. A real VZ boot remains a Mac integration check, not something inferred from Linux package success. |
| Mac release | `needs` resolved revision and successful image job; download that run's artifact directly, verify expected definition identity/checksums, stage it for build-app and package-macos. They consume local inventory, not a release URL that cannot exist yet. Release mode never silently builds or downloads a different image. |
| Publish | Existing app release gets `tsugumi-vm.tar.gz`, `vm-manifest.json`, checksums, `vm-licenses.tar.gz` (inventory.json, license texts, kernel config), and `vm-sources.tar.gz` plus checksum, together with existing app/CLI assets. One publish job after all artifacts/checks; no `vm-<hash>` tags/releases or publish-vm step. |
| Development download | Builder evaluates current aarch64-linux vm-boot identity using full filtered native/guest definitions (includes dirty/untracked intended sources as current local builder does). Reuse matching local payload; otherwise locate a successful CI artifact with that identity using `gh`/Actions artifact APIs, download and verify. Optionally use the matching manifest/payload on an app release; never choose latest solely by tag. Auth unavailable, artifact expired, or no exact match → local build. Document `gh` authentication; no new public hosting service. |
| Development local build | Retain `TSUGUMI_VM_BUILD=1` and Apple-container builder fallback; on a native/remote aarch64-linux builder run Nix directly. Build exactly the same vm output. Stage one payload+inventory with definition identity; update app/CLI scripts to call the resolver even if old files exist. `--check` and `--kernel` remain local builder utilities. |
| Retention | Keep release image/inventory/corresponding source assets while released binaries are distributed; Actions artifacts are an expendable development cache. Stop producing new vm-hash releases; do not delete historic releases/source assets. |

### Redistribution work (part of the image change, not deferred)

| Current code limitation | Required implementation / validation |
| --- | --- |
| vm-inventory.py inspects EROFS /store-paths and adds kernel separately | Retain that actual-image approach; accept explicit extracted image directory and evaluated derivation identity so Linux CI and local builds use one tool. Walk the full recursive derivation graph, record all runtime outputs and kernel; no boot-only package inventory. |
| `declared_licenses` assumes every non-OWN name is a nixpkgs attr | Resolve actual output owners, nixpkgs aliases and local derivations explicitly. Include Tsugumi's MIT license and own generated scripts, hosq and other flake-input sources; do not mark all unfamiliar outputs OWN. Existing kernel-config extraction stays. Unknown/missing source or license fails release inventory. |
| Fixed-output source collection misses local source filesets/flake inputs; BUILD_FILES has only nix + Container | Include exact Tsugumi build sources (all native component roots, protocol, SDK, embedded Agent/template inputs, CMake and Nix recipes), pinned flake inputs including hosq, upstream sources, fetched and nixpkgs-resident patches/configuration. Derive from actual build source inputs/manifests and compare membership; no replacement vm-source.txt to keep by hand. Source bundle must reconstruct the image from its unpacked tree, not rely on the checkout or an unrecorded input. |
| Source script currently `git archive HEAD` with small allowlist | Release uses immutable checked-out commit; local dirty builds must inventory/archive the same staged source the local Nix build used, never label HEAD as that binary's source. Include full recipes and rebuild instructions. Separately keep notices of Mac host/Swift/npm/formatters. |
| Boot-only README links to vm-hash release | Link inventory/source to app release tag, record image identity and pin; update THIRD_PARTY_NOTICES, LicenseAudit, RELEASING and asset lists. Bundle expanded license texts/inventory in app and CLI. Corresponding source is a release attachment, not runtime VM source. |
| Existing source archive has an explicit 2 GiB asset guard | Measure the full compressed archive on runner before deciding capacity. If it exceeds the existing per-asset limit, split **the source archive only** into numbered chunks plus checksums/reassembly instructions. One image payload is still fixed; do not silently omit sources. No speculative multi-image system. |

### Delete and replace together

| Delete | Consumers that must change in the same A image commit |
| --- | --- |
| `macOS/Container/vm-source.txt`; bundled `tsugumi-source` / CLI `vm-source` | build-app.sh, package-macos.sh; VMImage source/sourceEntries/sourceList and source staging; `tsugumi-source` virtiofs device, /source directory and mount. |
| `TSUGUMI_VM_SOURCE`, host `--vm-source` / `--vm-source-entry`, helper `--source` / `--source-entry` | HostRuntimeConfiguration, host main/options/vm_process, VMHost main, cli/tsugumi-macos, web/server main/environment, tests/doubles. Do not remove unrelated source-writing APIs or signal DispatchSource. |
| `/init` compile-on-boot source hash/build/cache path | vm-boot.nix, old vm-system comments, Swift/Web stage "building Tsugumi in the VM", EnvironmentManager/SharedVM stage mapping, localized strings/progress tests and docs. Keep a truthful supervisor-start stage. |
| `macOS/tools/publish-vm.sh`, `TSUGUMI_VM_RELEASES`, vm-hash URL construction | build-image.sh, app/CLI license fetches, inventory README/source links, release/static/license tests and documents. The separate repository route is already retired, but verify no active reference remains. |
| Existence-only local image reuse and boot-only vm-version contract | Resolver/manifest plus helper extraction tests; preserve explicit local build option, not old payload compatibility. |
| Static assertion that vm-source.txt covers Nix inputs | Replace with final image closure/source-bundle membership and no-runtime-source/compile-path checks; keep VERSION/build-machine-path checks over new source roots. |

## Commit schedule and worker allocation

All scopes include their matching tests/docs. Checks below are **future implementation checks**, not commands run for this planning task. Native target checks mean configure in the existing Nix dev environment, then `cmake --build build --target <target>`; run the indicated test binary through `tsugumi-test-env`/`tests/run-isolated.py`. This preserves isolation. Swift checks use isolated support/keychain doubles; any app launch uses background run-ui.sh and no dialogs.

A0 first partitions CMake and test source registration by concern without changing membership. Give each worker an exclusive fragment; workers do not edit a shared root/core/test manifest concurrently. Additions needing shared files must wait for the prior commit, not be merged by simultaneous writes. Existing test binaries may span multiple fragments; no requirement to invent a binary for each source split.

| Commit | Scope / primary files at existing paths | Depends on | One quick check | File-disjoint parallel assignment |
| --- | --- | --- | --- | --- |
| A0 `Prepare component build manifests` | Root/core/client/workerd CMake, tests/core/CMake and installed-core; establish per-concern fragments and include boundaries needed for following commits, preserving existing source/target/install sets. Prepare file-map schema outside repo. | Baseline | Build `tsugumid-mock`. | None; shared manifest coordinator. |
| A1 `Separate JSON and document contracts` | S2 + definition part of S3: schema, document/graph/types/resources, JSON caller includes, input_types/worker_condition, installed SDK JSON staging; assigned document/JSON fragments and their tests. Remove document → workerd_support link using neutral file/crypto helpers. | A0 | `tsugumi_input_types_tests`. | Serialize broad include updates first; A3 may start only on its exclusive TLS files. |
| A2 `Isolate worker values and module support` | S3 worker model/wire subset + S5, workerd runtime/local/remote code, core adapters/model/wire/module callers, worker/client fragments; SDK worker source install/default.nix changes as needed. Keep lifetime/slots unchanged. | A1 | `tsugumi_execution_service_tests`. | A3 only after exclusive TLS caller assignment; serialize A4 because both may edit core service/module consumers. Not A5 (workerd invocation/Nix). |
| A3 `Separate TLS from authorization policy` | S4: peer_tls, authorized-file reader and remote TLS caller, TLS fragment and peer_tls tests. No unrelated execution_broker edits. | A0 | `tsugumi_peer_tls_tests`. | A1; if A1 needs remote_main include edits, coordinator assigns those edits exclusively to A3. |
| A4 `Extract protocol-free transport` | S1: server/lanes, client transport options/impl, service and host service endpoint adapters, client/transport/service fragments, protocol/order tests. No wire schema changes. | A1 | `tsugumi_agent_rpc_tests`. | A3 only if its broker consumer needs no edit; serialize A2. Not A7 (service RPC), A9 (host service). |
| A5 `Separate store mechanisms from evaluation` | S6: nix_eval/derivation transport, workerd nix_store/invocation includes, vmd init and cache_usage sampler; narrow Nix/runtime-support fragments; evaluator doubles and installed support. | A2,A3 | `tsugumi_remote_result_tests`. | A7 after A4; A5 owns cache_usage, A7 owns versions. Not A6's run_scratch files until its interface is settled. |
| A6 `Remove scratch policy from file utilities` | fs/run_scratch default, explicit host launch options/environment, scratch test cases; separate measurement interface used by A5. No deletion/retention changes. | A1,A5 | `tsugumi_remote_result_tests`. | A7; no shared version/service files. Not A8 (environment). |
| A7 `Return typed version facts` | S10: versions/version_record, host/version_text/versions and tool presentation callers, project protocol graph/note fields, generated docs/vectors/Swift/TS bindings consumers, version test files. Use existing stopped-environment behavior. | A4 | `tsugumi_host_settings_tests`. | A5,A6; not A8/A9 shared contracts/generated consumers. |
| A8 `Move binary-cache policy to hostd` | S7: host settings/environment/VMProcess configuration, host protocol if needed, Swift BinaryCaches/settings/SharedVM and wire adapters; snapshot tests. | A6,A7 | Build `tsugumi-hostd` (then focused cache tests before integration). | A11 only if its shared TS adapters exclude environment/settings consumers. Not A9/A10/A12. |
| A9 `Centralize remaining credential decisions` | S9: host Settings/SecretStore/platform primitive, Provider/CoreHostClient/ApplicationSettings, web providerKeys/keychain, host protocol if needed, isolated secret tests. Preserve already-central policy; D1 determines minimal primitive placement. | A7,A8 | `tsugumi_host_settings_tests`. | No guaranteed parallel partner; owns cross-platform credential contracts. |
| A10 `Separate Swift VM and RPC products` | S8/S11: Package manifests/locks, RPC/ClientC/plugin, VMHost/VMCtl/Launcher value imports, CoreTests allocation, helper entitlements, build-host/build-app/build-vmctl/package-macos wiring at existing roots. VM and RPC package staging dirs remain under macOS until B. | A8,A9 | `swift build --package-path macOS/VM --product tsugumi-vmhost` (new prepared package). | A11 after exclusive TS files assigned; no common manifests/scripts. |
| A11 `Separate Web shared contracts from browser and server` | S11 TS: tsconfig/project refs and scripts, server shared imports, src/shared/i18n split, tests and nix/web filesets; no credential/environment ownership edits. No React in shared wire target. | A7,A9 | `nix build .#checks.aarch64-linux.web`. | A10; reserve web/server main/environment and CLI scripts for A12. |
| A12 `Ship the release-built guest closure` | Entire #293 contract above: packages.nix, Container boot/system/builder, inventory/source tools, release/CI, app/CLI payload packaging, host/helper source-option removal, VMImage/archive install, Web launch/progress, notices/static/VM tests; delete vm-source/publish-vm. All producers/consumers switch together. | A5,A10,A11 | `nix build .#packages.aarch64-linux.vm` on Linux builder. | None; cross-cutting integration commit. Keep code review sections by image/inventory/CI/consumer, not partial runnable commits. |
| A13 `Close the pre-move integration baseline` | Finish S11 component manifests/installed test support and package source lists; remove interim core-cycle links/exposed headers; fix only residual split integration. Freeze source membership, products, test inventory and resources for B. | A1–A12 | `nix build .#ci-core`. | None. If a residual behavior change is needed, separate it before this checkpoint. |
| B1 `Move components to their program owners` | One pure git-mv commit using frozen file map, including manifests; zero content edits/deletions/additions beyond renames. Same blob IDs/modes. | A13 | Mechanical blob/mode/map comparison only; **not buildable alone** (D5). | None; repository-wide move barrier. |
| B2 `Update paths for the program layout` | All integration rows below; path/include/package identity changes only. No source-list policy or protocol/behavior changes, no pin/version churn. | B1 | `nix build .#ci-core` (full matrix follows). | None; same integrator as B1. |
| C1 `Enforce native ownership boundaries` | Component include/link/install manifests, tests/static dependency checks + small negative-include/configure probes, installed-source/export checks; update CI invocation. | B2 | New `tsugumi-boundaries` CMake check target. | C2 (different native vs Swift/TS manifests and test modules). Root CI invocation belongs C3. |
| C2 `Enforce Swift and Web dependency boundaries` | Swift package graph/import test, TS resolved-import/browser metafile check; dedicated static modules. No new ownership refactor. | B2 | `nix build .#checks.aarch64-linux.web`. | C1. |
| C3 `Gate the integrated layout and image` | CI selector/result gate integration, final docs/maps and release/source reconstruction instructions; only shared gate files. | C1,C2 | `tests/check-fast.sh` (required before push). | None. Full acceptance matrix below before merging. |

Parallel waves (integration order remains the table): A0 → A1 with A3 → A2 → A4 → A5 with A7 → A6 → A8 → A9 → A10 with A11 → A12 → A13 → B1+B2 → C1 with C2 → C3. Explicitly named overlap exceptions above require exclusive file assignment, not conflict resolution after simultaneous edits. No separate worker should change prompts, operations, execution ownership or VM lifecycle during the move barrier.

## B2 path-bearing integration ledger

Behavioral #293 edits have already happened in A12. This table includes surviving files and newly prepared component files; it does not put deleted vm-source/publish-vm into the new tree.

| Surface / concrete paths | Path-only work and verification |
| --- | --- |
| Root and component CMake; cmake/embed*.cmake | add_subdirectory/include/source roots, embedded prompts/templates, owned public include prefixes, generated BFBS paths, install lists, testSupport imports. Preserve layout FP flag, C++ standards, warnings, SDK ABI, product names. Core OFF/Wasm must not need native/Nix. |
| tests/core/CMakeLists + cmake/installed-core.cmake | `_core_main`, private test header roots, `_local_service_source`, `_workerd_headers`, evaluator double and archive install/import paths; guide resources and operation driver. Tests linking host+core do not grant that dependency to production. |
| nix/core.nix, core-tests.nix, client-wasm.nix | Replace core/agent/ui-template paths with component filesets, guest vmd and rpc; independent installed support/SDK exports. Preserve local_worker.fbs in the complete schema list; any duplicate-list cleanup belongs to A, not this paths-only commit. |
| nix/{web,web-checks,cli,workerd,docs-site}.nix, nix/flake/* | npmRoot/importNpmLock/CWD/gen overlays; ui/shared assets; operationDriver still reuses ci-core archive; guest callPackage imports; flake/dev shell tools and docs player. Preserve output names/pins. |
| Guest vm/guest Nix/scripts/tools | Relative root depth/imports, build-image staging/dist path, inventory/source recipes, own-script provenance and rebuilt-source instructions. Archive identity legitimately changes if derivation paths change; rebuild at final layout, do not relabel old payload. |
| Source staging/archive manifests | CLI Linux git archive allowlist, native/Web/test Nix filesets, SDK installed worker sources, local builder and tests/ci-local staging. Final tar must evaluate flake outputs without obsolete paths. No all-repository source filter as a shortcut. |
| Swift Package/Plugin/ClientC/CoreTests/VM manifests | Flatc protocol root, client headers/default lib directory, package dependency identities (`macOS` → new names), shared resources and test support. All `#filePath` ancestor walks gain depth. Build VM/RPC without renderer dependencies; lockfile pins unchanged. |
| ui/macos/build-app + tools; hostd/tools/build-macos; vm/macos/tools; cli/package-macos | Native target output (`build-host/core/tsugumi-hostd` changes), independent Swift bin dirs/resource bundles, dylib install names, formatter resources, icons/help/math fonts, localization and VM payload licenses. Sign helpers using VM-owned entitlement file; preserve development identity/release signing policy. |
| Web scripts/package/tsconfigs/tests | Repo ancestor depth, docs/icon/player imports, ui/shared modules/templates, Wasm output/build script location, Node/npm CWD, generated bindings, native operation-driver location. |
| Web server main/coreHost/paths/environment/mcp; src/shared | Checkout `ui/web/dist` root differs from installed `web/dist`. Separate explicit checkout roots from injected installed roots; native host/core fallback output paths, SDK/demos/Agent catalog. Preserve serverBuild's runtime identity and old-server refusal. No new automatic shutdown of other installations. |
| agent Shared generator → hostd/agent/Shared/generate.py | Actual current outputs: Swift Shared.swift and demos/reference/cards.md; HERE parent depth, banner and SDK source path. No obsolete MCP_OUT assignment. Generate in disposable staging; `--check` and byte comparison except declared paths. |
| Protocol/docs/vector tooling | protocol/generate_docs.py, tests/tools/generate_service_vectors.py and current contract/tool/wire/number vectors: path references only. A7 owns semantic schema changes; freeze post-A fixtures before B, never regenerate to hide a B regression. |
| SDK codegen/default.nix | Scaffold ui/shared template paths and installed worker JSON/text/includes. Build foreign-system worker from installed SDK only, with checkout inaccessible. |
| .github workflows/actions | Commands, reusable image workflow paths, native/Web/Swift cache inputs and new VM/RPC package manifests/resolved/source hashes, upload/download names. Exact release revision and same-artifact handoff preserved. |
| tests/ci/select_jobs.py + test_ci_selection | Specific ui/macos/ui/web/ui/shared rules before generic ui; vm/macos/guest, util/rpc, tsugumid/document, hostd/agent. rpc affects native/Wasm/Web/player; guest affects VM/host/SDK/Swift; Agent catalog affects native/Web/Swift/prompt. Keep unknown=ALL, no-renames diff and prerequisite/result semantics. Run full CI for combined move. |
| tests scripts and suite paths | check-fast/ci-local/ci-remote-worker/run-isolated, core/python/remote/bench/prompt roots, source scans/binary defaults/CWDs/fixtures. Tests stay tests; do not recreate removed tests or pin prompt wording. |
| tests/static scans | Agent/CI/en_strings/host-state/licenses/marimo/history/release/templates/first-start/script-launch path assumptions. Assert expected scan roots are nonempty; VM tests now assert image/source bundle coverage, not runtime source list. |
| docs/site player/build/harvest/svg and src/lib/tests | WEB constant/npm imports, source-link mapping, bundles/help/player assets; keep published pages, URLs, demo data and player-data routes unchanged. |
| Plugins and CLI | marketplace/plugin discovery unchanged; checkout MCP fallback → ui/web/dist, installed fallback unchanged. install scripts/help/release helper references, Linux source archive membership. Test copied plugin/CLI outside checkout. |
| Docs/instructions | AGENTS map + background run/build commands; README, RELEASING, notices; docs HostPresentation/INSTALL/LicenseAudit/Versions/Visualization/WebUI/VMStoreHealth/features/security/claude-code; tests/bench/tool READMEs and optimize-tsugumi-prompts skill paths. Philosophy only link repair. No new prompt content. |
| Ignore/attributes/symlinks | Swift .build/.swiftpm caches in all packages, generated grammar paths/modes, case-sensitive Linux paths, CLAUDE symlink. No retained obsolete core/macOS/web/agent directories or compatibility links. |

Search all tracked/hidden text and constructed paths: slash literals, split path joins, parent indexing, CWD assumptions and build output names. Use the frozen file map to distinguish repository prefixes from Card/project paths or historical citations; never globally replace words such as core/web in datasets or documents.

## C: enforced dependency graph

| Owner | Allowed production dependencies / enforcement |
| --- | --- |
| util | Standard/OS/narrow external mechanisms only, no protocol/domain/stateful policy. Dedicated owned includes; compile without generated protocol include path. JSON helpers contain no Tsugumi schemas. |
| rpc | util + transport/TLS libraries; no generated Project/Host/Agent/worker message imports, Card names or filtering. Native/Wasm transports independently buildable. Protocol-aware Swift wire adaptation lives separately under client/swift, never in this generic transport target. |
| document | util, protocol, external type/schema parsers; no scheduler/history/worker lifecycle/evaluator/hostd. Own public headers only. Definition worker conditions/input ends allowed; live worker selection stays core. |
| tsugumid | document, util, rpc, protocol, worker value/transfer support; no hostd or Agent implementation. Project endpoint adapter private. No globally exported source root. |
| hostd/agent | document is the only tsugumid dependency; util/rpc/client/protocol/worker wire contracts allowed. No private version records, run scratch or core archive, directly or transitively. |
| workerd | util/rpc/protocol/SDK and narrow definition/value/store contracts; no project service/scheduler/store/history archive. Check executable and object-library link closure, not only direct includes. |
| vm/guest | narrow Nix/store/accounting support, util/protocol; no full core/evaluator link for vmd. Runtime payload may contain tsugumid executable without linking it into vmd. |
| Swift UI / VM / RPC | Independent VM package depends on Foundation/Virtualization and minimal client/swift RPC and VM value targets, never AgentClient/SwiftUI/renderer. UI uses Host RPC adapters, VM helper owns mechanics. One generator/plugin and no duplicated DTO/lifecycle implementation; protocol-aware Swift wire adaptation belongs under client, not generic rpc. |
| Web | Shared generated/wire/catalog resources import neither browser model/React nor Node server. Browser and server are separate projects with explicit shared references; resolve relative/type/dynamic imports too. Browser bundle metafile must contain no Node server module. |
| Builds/tests | Validate resolved include paths (including relative escapes), target source ownership and transitive links using CMake graph/compile commands; static early tests plus negative include probes. Imported testSupport and Nix staged sources obey same production graph. Deliberate integration test access is test-only. No broad permanent exception for current violations. |

## Implementation acceptance and review evidence

| Gate | Required evidence after implementation |
| --- | --- |
| A baseline | Clean configure/build of native products, client/Wasm, host, worker, vmd and independent Swift packages. Focused operation/exclusion/worker/lifetime/version/cache/credential tests; VM/image integration below. Record test names/source sets/resource digests at A13. |
| B1 mechanical proof | External before/after map from `git ls-files -s`; bijection of paths, identical blob IDs/modes and symlink meaning. 100% rename summary, no content delta. Compare against A13, not old main. |
| B2 semantic equivalence | Normalize only declared source/build prefixes in compile/source/link/install manifests and test lists. Compare resources/templates/prompts/demos/vector bytes; no schema/ABI/prompt-prose changes. Do not promise identical ELF/Mach-O/app signatures or Nix store hashes: debug paths, package identity and signing metadata change. |
| Final automated checks | Full tests/check-fast.sh before push; normal CI matrix (aarch64/x86 Linux + Darwin), docs/site/player, client-wasm, installed SDK/CLI/plugin smokes, Swift UI/CoreTests/VM/RPC checks. Use existing isolated runner. No real-model prompt evaluation for path changes. |
| Guest proof | Fresh isolated VM with no source share: prebuilt supervisor ready and compiled binaries present, no boot `nix build`/source-flake evaluation; terminal and real Card execution with workerd slots/descriptor handoff. Nix may build the Card. Test with network blocked and x86=none/native smoke; separately exercise Rosetta/qemu when available. |
| Store/lifetime proof | Clean stop skips dirty verification as before; interrupted stop checks/repairs or reports failure; image upgrade checks full closure. Quit/TERM ends host/helper/guest in order. Test update with existing history/store, reset retention, concurrent projects/shares. Use only temporary test stores/support, never user's real VM. |
| Packaging proof | Signed copied app and CLI outside checkout/no host Nix; codesign/entitlements/dylib checks, app/localized resources/help/module/player coverage; app and CLI manifest identify same image. Background run-ui only. Verify failed/missing/mismatched image or inventory blocks release. |
| Source/license proof | Extract image and reconcile all store paths plus kernel to inventory; inspect generated own/input/library licenses. Rebuild from extracted corresponding sources/recipes without checkout to verify complete source membership. Record compressed image/source/app sizes and runner build/cache timings. Missing tools/hardware checks remain explicitly incomplete. |

## Remaining owner decisions (refreshed Q2–Q8)

| Decision | Recommendation (one line) |
| --- | --- |
| D1 / old Q2: Which process hosts the minimal signed Keychain primitive? | Keep the currently signed UI/platform primitive if moving it changes ACL/prompt behavior; hostd owns policy through an explicit narrow adapter. Prefer hostd-native Security.framework only after isolated signed access verification; no extra daemon or shell workaround. |
| D3 / old Q4: Where does stateful guest/core usage accounting live? | Keep a narrow tsugumid/runtime_support target shared with vm/guest, with store mechanisms in workerd/nix; hostd cannot link it and util contains no sampler/policy. |
| D4 / remaining old Q5: Where should the shared protocol-aware Swift wire package live? | Use client/swift for RPC/ClientC/FlatcPlugin consumed by both VM and UI, so rpc remains transport-only; keep unspecified support roots and cli/release.sh as mapped. |
| D5 / conflicting commit constraints | Approve B1+B2 as one buildable checkpoint with two review commits; if every individual commit must build, squash them and review the rename artifact separately. |

Old Q1 is settled: one PR. Old Q3 does not require a new owner decision: current explicit-stop behavior stays; type the existing graph/note RPC facts without adding an auto-start or read-only core lifecycle. Q6 is settled: include #293 in A, one image payload, existing x86 behavior. Q7 is superseded by landed atomic operations and workerd local-slot ownership. Q8 needs no owner decision: no missing Development.md reconstruction and no empty future implementations. No compatibility or retention decision is reopened.

## Owner's direction (2026-11-17, replaces the A/B/C phasing above)

The goal is the separation of responsibilities; the directory layout is how the separation is made certain. So the work is NOT split into "split at existing paths", "pure move" and "enforce" phases. Each responsibility is separated and placed in its final directory in the same step, with its build manifest, and enforced as soon as it stands on its own. When the work shows a better structure than this plan, shift to it and record the change and the reason here. The destination ledger and split ledger above remain the starting map, not a contract.

## Integrated implementation checkpoint (2026-11-17)

**Status: the native tree builds, but the responsibility relocation and image acceptance are not complete. No source moves or index changes have been applied.**

### Repairs made before relocation

- Read the interrupted WIP patches (`git log -p -1 5fe4dbff`, `git log -p -1 f485e33a`) and retained the merged attached-Card/project-copy, space-key and decoding fixes.
- Removed two obsolete `using namespace agent` directives left after generic JSON moved out of Agent.
- Supplied the Agent RPC fixture's owned Host headers for its production result-budget constants without linking the Host implementation. Added the runtime schema include needed by the encoding tests.
- Updated the remaining version RPC assertions to typed commit/change facts. Arbitrary human-authored commit messages remain opaque text; private persisted version records are still checked separately.
- Made environment shutdown idempotent using its existing stop source. All three VM lifetime cases crashed after the WIP transport extraction: destruction called `Environments::Impl::Shutdown` again, whose notification reached the already-destroyed server. The macOS crash report identifies `Shutdown -> Stop -> Notify -> HostEvents::Publish -> Server::reply -> TransportServer::post`. The existing lifetime tests reproduce this and now pass; no new lifecycle mechanism or retention behavior was added.
- Replaced the UI script launcher's `compgen` with Bash prefix expansion. The flake's Bash lacks `compgen`; the three existing mocked-launch tests had lost the trust/state environment arguments. All static tests now pass. No app was launched.

### Evidence from this checkpoint

| Check | Result |
| --- | --- |
| `nix develop -c cmake --build build -j 10` | Passed after the final repairs, including all native products and gtest targets. |
| `python3 protocol/generate_docs.py`, `python3 agent/Shared/generate.py`, `nix shell --inputs-from . nixpkgs#flatbuffers -c python3 tests/tools/generate_service_vectors.py` | Completed; generated tracked files have no delta. |
| Isolated static suite | 77 passed after the launcher repair. |
| Native CTest, excluding `core_rpc` | Baseline: 756 passed, 7 skipped, 16 failed out of 779. Twelve failures require Seatbelt and report `sandbox-exec: sandbox_apply: Operation not permitted`; three were the lifetime crashes fixed above. `ModelCatalog.ExactRolesOrderAndBudgets` remains a real failure: Helper catalog 13614 bytes versus the existing 13600-byte budget. The budget and prompt text were not changed. |
| Isolated `tsugumi_vm_lifetime_tests` after repair | 4 passed. |
| Isolated `tests/core/test_versions.py` after typed-fact assertion repair | 11 passed. |
| Isolated `tests/python/test_vm_artifact.py tests/python/test_vm_store_health.py` | 14 passed. |
| Web `npm run gen`, `npx tsc --noEmit`, isolated `npm --prefix web test` with the production edit driver | Passed; recorded Keychain OS commands remained empty. |
| `nix eval --raw .#packages.aarch64-linux.vm.drvPath` | Passed. This is evaluation only, not a built or booted image. |
| Complete `core_rpc` CTest | Timed out at its existing 300-second limit, after reporting progress through about 30 percent. It is not a passing full Python suite. |
| Swift helper build | Could not compile the package manifest: SwiftPM's Seatbelt invocation reports `sandbox_apply: Operation not permitted`. No sandbox-disabling workaround was attempted. |
| Isolation cleanup | Each isolated run reports that `ps` is forbidden. Concurrent baseline runs also made the global escape detector observe other test/build temporary files; clean cleanup/isolation is not established. |
| App, VM, Docker, image/source rebuild and copied signed packaging | Not run: unavailable in this sandbox. |

### Blocking condition and exact next work

The first requested `git mv` was refused while creating `/private/var/piagent/work/tsugumi/.git/worktrees/strange-margulis-89f7c0/index.lock` (`Operation not permitted`), although the Git directory is advertised as writable. The source file remained at its original path and the index is unchanged. The owner has been asked whether ordinary filesystem renames are acceptable; that answer is still required before replacing the explicitly requested `git mv` workflow. No `git add` or commit was issued.

1. Resolve the move constraint, then separate and relocate each responsibility together with its manifest and dependency enforcement. The original destination/split ledgers remain applicable; no final-layout deviation has yet been implemented. Draft native move/manifest scripts exist in `/tmp/layout-native.py` and `/tmp/layout-manifests.py`; they were not applied and are **unverified drafts**, not a buildable implementation.
2. Complete util/JSON, generic RPC/TLS/client transport, document, worker values/store/authorization and narrow runtime support in their final roots. Remove broad/private source exports. Host needs generic byte transport, not the project notification server; keep project subscription/visibility/sequence state in the project endpoint.
3. Complete hostd/agent, Host cache/credential policy and the remaining project runtime; split the independent `client/swift` and VM packages, then move UI/shared/Web/Mac resources and repair checkout-versus-installed paths.
4. Complete the guest image in `vm/guest`, VM mechanics in `vm/macos`, source/license reconstruction, release artifact handoff and final-layout image identity. Preserve D1, D3, D4, one payload and the existing x86 choice.
5. Update SDK/CLI/Nix/tests/docs/CI source sets and path construction, add the planned dependency gates, resolve the catalog-budget failure, and run the acceptance suites in the final layout. Real VM/app/Seatbelt/packaging checks still need a capable environment.

## Active continuation (2026-11-17)

The owner resolved the move constraint: use ordinary filesystem renames and
leave the index untouched. This supersedes the preceding blocking condition.
No `git add` or commit is permitted. The earlier repairs remain in the tree.

Native responsibility relocation is in progress: physical owned headers in
`util`, `rpc`/`rpc/tls`, `tsugumid/document`, `workerd/nix` and
`tsugumid/runtime_support`; private Host/Agent headers under `hostd`; project
runtime under `tsugumid/runtime`. Host now uses `TransportServer` directly,
without compiling the Project notification endpoint. Text validation belongs
to util (filesystem paths also use it), while JSON links that mechanism.

Nix commands for the uncommitted relocation must use a path source/snapshot:
Git flakes omit new, untracked destinations until the owner stages them.
This is a source-selection issue, not evidence of a missing component.

### Integrated layout decisions and implementation

- Native components now live at their final roots, with physical public include
  trees and private implementation headers. The CMake gate checks source
  ownership, include roots and production links. The static include gate also
  resolves relative escapes; integration-test access remains test-only.
- Reflection bytes shared by Host and Project belong to `protocol` as
  `tsugumi_rpc_schema`, rather than making Host compile a Project endpoint or
  link its runtime. `rpc` contains only generic transport/dispatch/TLS.
- The macOS helper package is `vm/macos/runtime`. A package directly at both
  `ui/macos` and `vm/macos` would give the two local dependencies the same
  SwiftPM path identity (`macos`); the nested runtime root makes their
  identities distinct. `TsugumiVMTypes` uses Foundation only; the UI consumes
  values, while `TsugumiVMRuntime` owns Virtualization and the helper.
- `tsugumi-vmctl` belongs to `cli/macos`: inspection showed that it is a Host
  RPC client using `EnvironmentManager`, not a VM owner. Its build tool builds
  and signs the independent helper beside the diagnostic client. It does not
  bring AgentClient or renderer dependencies into the VM helper package.
- D1 remains unchanged: the Keychain primitive stays in the same signed UI
  product. Host-owned credential/source policy continues through its narrow
  provisioning adapter. D3 is `tsugumid/runtime_support`; D4 is `client/swift`.
- Binary-cache validation, persistence and next-boot snapshot policy moved from
  Swift Launcher to Host. The helper materializes the supplied bytes only
  when constructing a new VM. The existing validation/privacy/frozen-snapshot
  cases and private-cache Nix probe are being ported to the native owner.
  New Host/VM adapter fields were regenerated with the wire fixtures.
- The guest build, provenance and redistribution tools are in `vm/guest`.
  The image remains one archive with the kernel and EROFS attachment; boot
  starts the prebuilt runtime and has no source share or Tsugumi compilation.
  Rosetta / explicit qemu / none is unchanged.
- Web shared wire/catalog/value resources are under `ui/web/shared`, separate
  from browser adapters in `src/wire`. In particular the Host scene codec no
  longer imports the browser HTTP API or browser preferences. Shared, browser
  and server have separate TypeScript checks and resolved-import gates; the
  browser bundle checks its input metafile for server modules.
- The merged Card-copy contract puts helper catalogs at 13614 bytes, 14 above
  the old test ceiling. The test ceiling is now 13700, preserving a small
  margin. No Agent prompt or tool-description prose was shortened to satisfy
  a byte counter; the merged copy and space-key behavior remains.

The earlier checkpoint's "no source moves" statement is historical. Final
validation is still in progress; neither a booted guest nor a signed copied
application has been claimed from this sandbox.

Release CI now uses the existing multi-project/worker-execution VM scenarios
against the downloaded image after assembling the signed app. This addresses
the concrete #293 boot/runtime failures: compiling Linux binaries alone did not
exercise the shipped image or descriptor handoff. The smoke can catch those
startup/execution failures, but does not prove offline boot, image upgrade/store
repair, all x86 modes, or source-bundle reconstruction. It has not run here.

The sandbox grants read-only access to `.agents`. Its
`skills/optimize-tsugumi-prompts/SKILL.md` therefore still names the old Agent
and Web paths. The exact path-only repair is prepared in
`plans/layout-protected-skill-paths.patch`; applying it requires a writable
instruction directory. No permission bypass, index mutation or commit was used.
