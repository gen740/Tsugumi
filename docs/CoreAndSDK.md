# The core and the Card SDK

**Status:** decided by the owner on 2026-11-13 and 2026-11-21; not implemented yet.
Each section states what is decided and what the code does today. Points that are not decided yet are listed under [Open](#open).

The Card ABI that connects the two is in [Card ABI 9](CardABI.md), and what a Card sees when it runs is in [The sandbox contract](SandboxContract.md).

## The core knows no language

**Decided.**

- The core is `tsugumid`, `workerd`, `hostd`, `protocol` and the UIs. None of them knows a programming language.
- A Card SDK is a swappable part, chosen per Card. It will later move to its own repository, so that users can build their own SDK.
- A Card selects its SDK in its `default.nix`, by the SDK that gets injected into it, not in `_card.json`. This leaves room for third-party SDK implementations without tying the Card document to Nix evaluation.
- The contract between the core and an SDK is the common [Card ABI](CardABI.md).
- The worker comes from the core, including the build of a worker for a foreign system or a remote host (decided 2026-11-22). This revises the 2026-11-13 rule that a Card brings its worker from its SDK commit. Across versions, the promise is the Card ABI.
- Bash-bodied Cards, `preRun` and `postRun` are abolished for now. If they return, it will be as an SDK extension for each language that needs them, not in the core.

**Today.**

- `_card.json` names a `language` from a fixed list (`python`, `cpp`, `go`, `rust`, `csharp`, `bash`, `r`, `julia`, `ruby`; [Code Card settings](CardGuide.md#code-card-settings)).
- The kernel runs a Bash Card's program directly, and Package Cards load `lib/main.so` ([Another system](features.md#another-system-x86_64-linux)).
- For another system, a Card's build makes the worker (`worker` of the Nix SDK) from the Card's own nixpkgs ([Another system](features.md#another-system-x86_64-linux)).
- `preRun → run → postRun` are steps that the worker runs ([Worker state](CardGuide.md#worker-state)).
- hostd fixes one formatter per language ([Host presentation services](HostPresentation.md)), and `InputTypes` takes a `language` used to render SDK definitions ([Protocol](Protocol.md#inputtypes)).
- The SDKs live in [sdk/](../sdk/) of this repository, with the [Nix SDK](../sdk/nix/default.nix).

## What the core evaluates

**Decided.**

- `manifestFor` is abolished. The core's `entry_nix` evaluates the Card's attributes directly:
  - `run`: one or more derivations containing `lib/main.so`, plus their labels;
  - `views`;
  - `exports`.
- An SDK's Nix entry only returns the derivation.
- This removes the manifest JSON that is defined in two places today.

**Today.** `entry_nix` in `tsugumid/runtime/src/nix.cc` calls `tsugumi.manifestFor` ([sdk/nix/default.nix](../sdk/nix/default.nix)). The SDK writes the manifest JSON, and `schema::CardManifest` decodes it again (`nix.cc:773`).

## What the core needs from an SDK

**Decided.** There is no SDK metadata. The interface between the core and an SDK is only:

1. **a Nix build entry** that returns a derivation containing `lib/main.so`;
2. **[Card ABI 9](CardABI.md).**

What an SDK might otherwise have described lives elsewhere:

- Highlighting by file extension is UI presentation.
- Formatting is the Agent's job, or the Card's `default.nix` can expose a formatter.
- The guide is ordinary documentation in the SDK's repository.
- The generated header (`card.hh`) is the description of the types.
- These are removed from the core:
  - hostd's `DescribeInput`;
  - the `FormatLanguage` enum;
  - the per-language formatter tables.

**Today.**

- hostd renders input types per language with `sdk::DescribeInput` (`hostd/src/input_types.cc`).
- `FormatLanguage` in `protocol/tsugumi_enums.fbs` lists `cpp`, `python`, `bash`, `nix`, `json` and `fbs`.
- hostd fixes one formatter per language ([Host presentation services](HostPresentation.md)).
- The guide is in the Agent's prompts and in [CardGuide.md](CardGuide.md).

## Templates

**Decided.**

- Templates are independent of the SDK, and of the core and the UI.
- There are template stores at two levels:
  - the host, under Tsugumi's support folder;
  - the project: `templates/` in the project, versioned with it.
- When names collide, both are listed, each labelled with its store. Neither shadows the other.
- A template is a folder of Card definition folders. It may hold several Cards or a partial graph, plus an optional `layout.json` fragment. IDs are remapped when it is copied.
- Users can add templates.
- A "make template" skill may create templates at the project level.
- Templates can be imported from git, given a URL, a ref and a subdirectory. v1 supports public repositories only.
- The core manages the list: it stores, lists and copies templates, and imports them from git.
- The Agent or the user chooses a template. The core never chooses: there is no automatic selection and there are no per-language defaults.
- The Agent may list and copy templates. Adding or importing one needs a person's approval.
- Scaffolding a new Card is the Agent's job.
- The "New Card" menu offers:
  - Draft, Trigger, Input, Branch, Fork, Merge and Collect;
  - "From template…", with Project and Host sections and nothing preselected.
- No templates ship in the core. Example templates live in the separate SDK repository, which will be split out later.
- These are removed:
  - the fixed language lists;
  - `ui/shared/templates`;
  - the language presets;
  - the button that writes a `pythonCard` `default.nix`.

**Today.**

- `language` in `_card.json` is a fixed list ([Code Card settings](CardGuide.md#code-card-settings)).
- [ui/shared/templates](../ui/shared/templates/) holds one scaffold per language.
- The Card log view has a button that writes a `pythonCard` `default.nix` (`ui/web/src/ui/cardLog.tsx`, `ui/macos/Sources/App/Detail/CardLogViews.swift`).

## Displays in the UI

**Decided.**

- Every display is a view module ([Card ABI 9: the display form](CardABI.md#the-display-form)). There are two kinds:
  1. **Wasm + WebGPU only.** The host runs these natively: a Wasm runtime plus native WebGPU (Dawn or wgpu-native), with no WebKit.
  2. **JS/DOM modules**, which run in WebKit.
- The core ships default view modules on the UI side for text, markdown, scalar, table, image, points, line chart, Plotly and Vega-Lite. They move out of `sdk/nix/builtin-views.nix`.
- Table, image, line chart, text, markdown, scalar and points are kind 1 defaults. Plotly and Vega-Lite are JS libraries, so they are kind 2 defaults.
- Until the native host exists, the kind 1 defaults ship as JS modules in the WebKit host. Wasm versions replace them later (decided 2026-11-22).
- This brings Dawn back. It reverses the removal of Dawn in [#225](https://github.com/gen740/Tsugumi/pull/225).
- The native renderers are removed: `ui/web/src/ui/display.tsx`, `DisplayContent` and `PointCloudView` in the Mac app's `CanvasItems.swift`, and `TablePage`.
- The contract for JS/DOM modules (kind 2):
  - The host hands the module one root element plus the data, and the module draws freely.
  - The only convention is an update event when new data arrive: a `CustomEvent` named `tsugumi:data`, dispatched on the root element, whose `detail` is `{config, blobs}`. The module listens for it if it wants.
  - Resizing is the module's own business, for example with `ResizeObserver`.
  - The host disposes of a module by disposing of the module's isolated context. Whether that context is an iframe is part of the host-architecture proposal under [Open](#open).
  - The `initialise` / `update` / `resize` / `dispose` / `snapshot` function contract goes.
- There is no output frame on the Canvas and no display summary on the Card's face. These are removed:
  - `OutputFrameView`;
  - `displays` in `layout.json`;
  - `DisplayGeometry` in `host.fbs`, and hostd's `DefaultDisplayGeometry`;
  - the frames of `LayoutScene`;
  - the whole thumbnail machinery;
  - the `displaySummary` line on the Card's face.
- Displays appear only in views: the expanded window, run comparison and chat evidence.

The measurements behind the two kinds were taken on an M3 Max with macOS 27.2. Each view drew the same table at 1200×800 @2x, and the footprint is summed over every process involved:

| | Native | WKWebView + iframes, same Wasm | WKWebView + iframes, HTML | One WKWebView per view |
| --- | --- | --- | --- | --- |
| N=20, idle | 162 MB | 759 MB | 757 MB | about 1130 MB |
| N=50, idle | 381 MB | 1799 MB | 967 MB | about 2760 MB |
| N=20, first frame | 85 ms | 482 ms | 409 ms | about 1000 ms |
| N=50, update round | 9.5 ms | 33 ms (vsync) | 156 ms | — |

- Right after drawing, the gap narrows to 2–3×, because the Metal driver pool sits in the host or in the GPU process either way.
- The native figures exclude compositing and process start.
- The native text matched an HTML table at 2x.
- The Card's description, which a person sets, is unrelated and stays.

**Today.**

- `ui/shared/modules/viewer.js` calls a module's `initialise`, `update`, `resize` and `dispose`.
- Displays are drawn on the Canvas in output frames, whose geometry hostd keeps in `layout.json`.
- The Web UI shows a `displaySummary` line on the Card's face (`ui/web/src/model/session.ts`).

## Order of work

**Decided.**

1. Remove language knowledge from the core.
2. Rebuild the SDK with C++ as the reference.
3. Make the SDK self-contained in this repository.
4. Split it out into its own repository.

- The default SDK ships with the core until the split.
- The migration to [Card ABI 9](CardABI.md) happens on `main`. C++ moves first; each other language is unavailable until it is ported.

**Today.** None of these steps has started.

## The bundled SDKs

**Decided.** These points are part of the plan for the SDKs bundled with Tsugumi, not constraints of the ABI.

- How values are mapped inside an SDK is that SDK's business.
- R, Julia and Ruby start by passing values as JSON inside their SDK:
  - Generated, typed C++ in that SDK's `main.so` converts FlatBuffers to and from JSON, with no reflection.
  - The interpreter uses its ordinary JSON library.
  - The ABI, and the values passed from Card to Card, stay FlatBuffers.
  - In R, integers beyond 2^53 become decimal strings.
  - NaN and Inf become fixed tokens.
  - `[ubyte]` becomes base64.
- The SDK may later switch to direct C-API glue without touching the ABI.

**Today.** R, Julia and Ruby return values that the worker converts from JSON to each port's FlatBuffer ([Building results](CardGuide.md#building-results)).

## Philosophy constraints that still hold

**Decided.** These hold through the whole change ([Philosophy](Philosophy.md#a-laboratory-notebook-and-who-decides-what-is-kept)):

- Run data travel only on `tsugumid`'s channel. This covers Tsugumi's own transport. I/O that a Card starts through its grants is a permitted side effect ([Grants](SandboxContract.md#grants)).
- Only a person deletes stored run data.
- Definitions (code and environment) may be shared by the machine.

**Today.** These already hold. See [Remote execution](RemoteExecution.md#data-left-on-a-worker) and [Where data is kept](security.md#8-where-data-is-kept-and-how-to-delete-it).

## Open

These questions are not decided yet:

- The name of the separate SDK repository, for example `tsugumi-kit` or `tsugumi-sdk`.
- **The native module contract**, the next design topic (from a Codex study):
  - a single `view_dispatch` entry, with `Init`, `DataChanged`, `Viewport`, `Input`, `Action` and `Frame`;
  - optional host services:
    - `text_rasterize`, through Core Text, Pango or Canvas2D;
    - `view_emit`, for `Semantics`, `SelectionText` and `ScrollExtent`;
  - Dawn in a helper process that can be restarted;
  - the same `.wasm` in the browser through a JS shim.
- **Proposed host architecture for JS/DOM displays:**
  - one shared WebView per window, run comparison or chat, with one sandboxed iframe per display;
  - iframes mount lazily and unmount when off screen, with about 8 live at once under LRU.

  The measurements behind it, on an M3 Max with macOS 27.2: 20 Plotly views took 1128 MB with one WKWebView per view, and 728 MB in one WKWebView with 20 iframes. An iframe with little in it costs about 0.7 MB.
- What to do without WebGPU (macOS before 26, WebKitGTK).
- A watchdog for a runaway module in the shared WebView process.
