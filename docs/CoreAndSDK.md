# The core and the language SDKs

**Status** — decided by the owner on 2026-11-13, 2026-11-21 and 2026-11-22. The ABI 9 cutover implements it.
Points that are not decided yet are listed under [Open](#open).

The Card ABI that connects the two is in [Card ABI 9](CardABI.md), and what a Card sees when it runs is in [The sandbox contract](SandboxContract.md).

## Terms

| Term | Meaning |
| --- | --- |
| **Card ABI** | The contract between the core and a language SDK: `tsugumi_main` plus `card.fbs` ([Card ABI 9](CardABI.md)) |
| **_language_ SDK** (C++ SDK, Python SDK, …) | The swappable part for one language: the Nix builder that produces `lib/main.so`, the code generation for the typed entry and `tsugumi_main`, the runtime library (`send`, `cancelled()`, `remaining_ms()`, and an embedded interpreter where one is needed), its views and its documentation |
| **tsugumi-kit** | The separate repository ([gen740/tsugumi-kit](https://github.com/gen740/tsugumi-kit)) that holds the default language SDKs and the example templates |
| **View** | A display surface that a Card declares: a name, a type (`html` or `dawn`) and one file ([Views](#views)) |

"SDK" always names one language's part. It never names tsugumi-kit as a whole or the worker, which belongs to the core.

## The core knows no language

- The core is `tsugumid`, `workerd`, `hostd`, `protocol` and the UIs. None of them knows a programming language.
- A language SDK is a swappable part, chosen per Card. Users can write their own.
- tsugumi-kit ships eight default SDKs: C++, Python, Go, Rust, C#, Julia, R and Ruby. Tsugumi pins one kit revision in its `flake.lock`.
- A Card selects its language SDK in its `default.nix`, from the kit that the core injects as `inputs.tsugumi`, not in `_card.json`. The core binds each builder to the Card's generated contract.
- The contract between the core and a language SDK is the common [Card ABI](CardABI.md).
- The worker comes from the core, including the build of a worker for a foreign system or a remote host. Across versions, the promise is the Card ABI.
- Bash-bodied Cards, `preRun` and `postRun` are abolished. If they return, it will be as an extension of a language SDK, not in the core.
- The Card's process gets no implicit `/bin/sh` and no implicit `PATH`. A Card declares the programs it runs to its language SDK's builder (for example `tools = [ pkgs.coreutils ];`), the builder installs them in the Card package's `bin` folder, and the worker sets `PATH` to that folder alone ([The sandbox contract](SandboxContract.md#the-worker-builds-what-the-card-sees)).

## What the core evaluates

The core evaluates a Card's `default.nix` directly. It may define only these attributes:

- `run`: a derivation containing `lib/main.so`, or a list of `{ label; run; }`;
- `views`: the Card's [views](#views);
- `exports`: values that downstream Cards may read.

A language SDK's Nix builder only returns the derivation. There is no manifest. The build plan holds the run derivations, the ordered view list and, for remote or foreign execution, the core's worker.

## What the core needs from a language SDK

There is no language SDK metadata. The interface between the core and a language SDK is only:

1. **a Nix builder** that returns a derivation containing `lib/main.so`;
2. **[Card ABI 9](CardABI.md).**

What a language SDK might otherwise have described lives elsewhere:

- Highlighting by file extension is UI presentation.
- hostd formats JSON and Nix, chosen by file extension. Formatting Card source is the Agent's job, or the Card's `default.nix` can expose a formatter.
- `InputTypes` returns the resolved FlatBuffers schema of connected ports. [Host presentation services](HostPresentation.md)
- The guide is ordinary documentation of the language SDK, in tsugumi-kit.
- The generated entry (for example `card.hh`) is the description of the types.

## Templates

- Templates are independent of the language SDKs, and of the core and the UI.
- There are template stores at two levels:
  - the host, under Tsugumi's support folder;
  - the project: `templates/` in the project, versioned with it.
- When names collide, both are listed, each labelled with its store. Neither shadows the other.
- A template is a folder of Card definition folders. It may hold several Cards or a partial graph, plus an optional `layout.json` fragment. Card IDs are remapped when it is copied.
- Users can add templates from a folder, or import them from a public HTTPS Git repository with a ref and a subdirectory.
- A "make template" skill may create templates at the project level.
- The core manages the list: it stores, lists and copies templates, and imports them from Git.
- The Agent or the user chooses a template. The core never chooses: there is no automatic selection and there are no per-language defaults.
- The Agent may list and copy templates. Adding or importing one needs a person's approval.
- Scaffolding a new Card is the Agent's job.
- The "New Card" menu offers Draft, Trigger, Input, Branch, Fork, Merge and Collect, and "From Template…" with Project and Host sections and nothing preselected. [Reuse Cards from a template](Usage.md#reuse-cards-from-a-template)
- No templates ship in the core. Example templates live in tsugumi-kit.

## Views

A Card declares its views at the top level of its `default.nix`, next to `run` and `exports`:

```nix
{ inputs, ... }:
let
  sdk = inputs.tsugumi.cppSdk;
in
{
  run = sdk.cppCard { src = ./src; };
  views = sdk.views ++ [
    { name = "notes"; type = "html"; blob = ./notes.html; }
  ];
}
```

- Each entry is `{ name; type; blob; }`. `type` is `html` or `dawn`. `blob` is one file in the Nix store: a self-contained HTML file for `html`, a Wasm binary for `dawn`.
- An SDK's own views are added explicitly with `sdk.views`. Nothing is merged implicitly. The C++ SDK's list holds html views for text, Markdown, tables, images, line charts (`LineChart`) and points (`Points`), and the Python SDK's list adds Plotly. Each view's bytes format is private to its SDK. The SDKs that had line charts and points before ABI 9 keep them as html views; a `dawn` version comes later. An SDK with an empty list sends to views taken from another SDK's list, or the Card declares its own.
- The core reads the list and sends it to the UIs (`CardViewsUpdated`) before any run. An empty list clears the Card's views. The core does not interpret names, types or blob contents.
- The UIs bundle no views. Each UI shows a Card's declared views in the Card's view window, routed by `type`, and draws nothing on the Canvas.
- A Card sends to a view with the ABI's `send(view, bytes)` ([Card ABI 9](CardABI.md#effects-and-their-lifetimes)). The core relays `{view, bytes}` without resolving the name. The UI matches the name against the Card's declarations and shows an error for an unknown one.
- Nothing in the display path is recorded. A view shows live data. Results that people check later are typed outputs, files and artifacts ([What a run records](CardABI.md#what-a-run-records)).

### html views

- The UI loads the file in an isolated frame with no network access (`connect-src 'none'`).
- It posts one window message, `{tsugumi: 1, port}`, which transfers a `MessagePort`.
- Messages on that port:
  - `{type: "data", bytes: ArrayBuffer}`, the bytes as the Card sent them;
  - `{type: "run", state: "started" | "ended"}`.
- The view draws its own empty state until data arrive, and owns its resizing.
- Disposing of a view disposes of its frame.

### dawn views

A `dawn` view is Wasm whose only imports are a pinned `webgpu.h` subset, under the `webgpu` import module. It exports:

```
tsugumi_view_abi() -> i32                     // = 1, checked first
tsugumi_view_alloc(size: i32) -> i32          // host copies message bytes here
tsugumi_view_start(device: i32, queue: i32, format: i32,
                   width: i32, height: i32, scale: f32) -> i32
tsugumi_view_message(ptr: i32, len: i32) -> i32
tsugumi_view_run(state: i32) -> i32           // 1 started, 0 ended
tsugumi_view_resize(width: i32, height: i32, scale: f32) -> i32
tsugumi_view_frame(target: i32) -> i32        // bit 0 = wants another frame
```

- A return of 0 means success. A negative return or a trap makes the host drop the instance and show the error.
- Message bytes are borrowed for the duration of the call.
- Fonts belong to the view.
- One instance per view module per host; views of the same module share it.
- The Web UI will run the same Wasm through a shim from `webgpu.*` to `navigator.gpu`.

Until the native host exists, the UIs show a placeholder for `dawn` views.

The measurements behind the native host were taken on an M3 Max with macOS 27.2. Each view drew the same table at 1200×800 @2x, and the footprint is summed over every process involved:

| | Native | WKWebView + iframes, same Wasm | WKWebView + iframes, HTML | One WKWebView per view |
| --- | --- | --- | --- | --- |
| N=20, idle | 162 MB | 759 MB | 757 MB | about 1130 MB |
| N=50, idle | 381 MB | 1799 MB | 967 MB | about 2760 MB |
| N=20, first frame | 85 ms | 482 ms | 409 ms | about 1000 ms |
| N=50, update round | 9.5 ms | 33 ms (vsync) | 156 ms | — |

- Right after drawing, the gap narrows to 2–3×, because the Metal driver pool sits in the host or in the GPU process either way.
- The native figures exclude compositing and process start.
- The native text matched an HTML table at 2x.

## Value mapping in the default SDKs

These points belong to the default SDKs, not to the ABI.

- How values are mapped inside a language SDK is that SDK's business.
- The R, Julia and Ruby SDKs pass values as JSON inside the SDK:
  - Generated, typed C++ in the SDK's `main.so` converts FlatBuffers to and from JSON, with no reflection.
  - The interpreter uses its ordinary JSON library.
  - The ABI, and the values passed from Card to Card, stay FlatBuffers.
  - In R, integers beyond 2^53 become decimal strings.
  - NaN and Inf become fixed tokens.
  - `[ubyte]` becomes base64.
- Each of these SDKs may later switch to direct C-API glue without touching the ABI.

## Philosophy constraints that still hold

These hold through the whole change ([Philosophy](Philosophy.md#a-laboratory-notebook-and-who-decides-what-is-kept)):

- Run data travel only on `tsugumid`'s channel. This covers Tsugumi's own transport. I/O that a Card starts through its grants is a permitted side effect ([Grants](SandboxContract.md#grants)).
- Only a person deletes stored run data.
- Definitions (code and environment) may be shared by the machine.

See [Remote execution](RemoteExecution.md#data-left-on-a-worker) and [Where data is kept](security.md#8-where-data-is-kept-and-how-to-delete-it).

## Open

These questions are not decided yet:

- **The native host for `dawn` views:** a Wasm runtime with Dawn, possibly in a helper process that can be restarted.
- **The host architecture for `html` views:** one shared WebView per window with one sandboxed iframe per view, mounted lazily and unmounted off screen, with about 8 live at once under LRU. On an M3 Max with macOS 27.2, 20 Plotly views took 1128 MB with one WKWebView per view and 728 MB in one WKWebView with 20 iframes. An iframe with little in it costs about 0.7 MB.
- What to keep from views in a run's record.
- What to do without WebGPU (macOS before 26).
- A watchdog for a runaway view in a shared WebView process.
