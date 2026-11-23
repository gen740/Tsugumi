# The core and the language SDKs

**Status** — decided by the owner on 2026-11-13 through 2026-11-23.
Points that are not decided yet are listed under [Open](#open).

The Card ABI that connects the two is in [Card ABI](CardABI.md), and what a Card sees when it runs is in [The sandbox contract](SandboxContract.md).

## Terms

| Term | Meaning |
| --- | --- |
| **Card ABI** | The contract between the core and a language SDK: `tsugumi_main` plus `card.fbs` ([Card ABI](CardABI.md)) |
| **_language_ SDK** (C++ SDK, Python SDK, …) | The swappable part for one language: the Nix builder that produces `lib/main.so`, the code generation for the typed entry and `tsugumi_main`, the runtime library (`send`, `cancelled()`, `remaining_ms()`, and an embedded interpreter where one is needed), its views and its documentation |
| **tsugumi-kit** | The separate repository ([gen740/tsugumi-kit](https://github.com/gen740/tsugumi-kit)) that holds the default language SDKs and the example templates |
| **View** | A display surface that a Card declares: a name, a type (`html` or `dawn`) and one file ([Views](#views)) |

"SDK" always names one language's part. It never names tsugumi-kit as a whole or the worker, which belongs to the core.

## The core knows no language

- The core is `tsugumid`, `workerd`, `hostd`, `protocol` and the UIs. None of them knows a programming language.
- A language SDK is a swappable part, chosen per Card. Users can write their own.
- tsugumi-kit provides ten language SDKs. A Project declares and locks the kit source like any other Nix dependency. The Card selects its language entry point in `default.nix`.
- The contract between the core and a language SDK is the common [Card ABI](CardABI.md).
- The worker comes from the core, including the build of a worker for a foreign system or a remote host. Across versions, the promise is the Card ABI.
- The Card's process gets no implicit `/bin/sh` and no implicit `PATH`. A Card declares the programs it runs to its language SDK's builder (for example `tools = [ pkgs.coreutils ];`), the builder installs them in the Card package's `bin` folder, and the worker sets `PATH` to that folder alone ([The sandbox contract](SandboxContract.md#the-worker-builds-what-the-card-sees)).

## The builder interface

The core calls a Card's `default.nix` with `{ params, inputs }`. The Card imports nixpkgs and its SDK from the [project's locked Nix search path](features.md#project-nix-search-path).

- `params` is the evaluated flat set of current values from [`params.nix`](CardGuide.md#parameters). The values are also sent at runtime in `CardInput.params`.
- The `inputs` attribute set is derived from the wiring and keyed by destination input port name.

| Upstream | `inputs.<port>` |
| --- | --- |
| Unconnected | Attribute absent |
| Code Card | `{ type; exports; }`, with `{}` for an undeclared `exports` |
| Branch, Fork or Collect | `{ type; exports; }` when the route preserves a Code producer, with `type` from the wiring and `exports` from that producer |
| Merge | `{ type; }`, including a generated union for mixed FlatBuffer types |
| Draft | `{ type; }`, with an empty provisional type |
| A built-in with no eligible Code producer | `{ type; }` |

The `type` field is the store path of a FlatBuffers schema with one input port. Its `inputs.Inputs` root has one field with ID 0, using the connected type. It includes the input type's dependencies in `inputs.<port>`. A Path field is a `string (tsugumi: "path")`.

Each kit entry point at `sdk/<lang>/default.nix` takes `{ pkgs }`. It exposes a language builder, an API builder and `views`. For example, the C++ entry point provides `cppCard` and `cppApi`. Both builders take `common = { inherit inputs params; outputs = ./_outputs.fbs; }`. The run builder also takes `src` and language-specific options. The builders generate the typed interface from the input schemas, the authored outputs and the parameter names and types. A standalone caller can pass `params = import ./params.nix;`. See the [temperature example](CardGuide.md#the-files) and the [kit builder contract](https://github.com/gen740/tsugumi-kit/blob/main/docs/sdk-contract.md).

### The generated API (`share/tsugumi/api/`)

The Card's top-level `api` attribute is a code-generation derivation. Its output contains the generated API and a `README.md` listing the types under `share/tsugumi/api/`. The run and API builders share code generation. Normal Card builds leave `api` unevaluated.

### Evaluating a Card expression

The [generic evaluation tool](features.md#evaluating-a-card-expression) runs a Nix expression in the saved Card's directory on the local system. It can return a JSON value or build one derivation and return its regular output files. **Build SDK** in the Mac and Web Card editors builds `(import ./. { inherit params inputs; }).api` with that tool and shows `share/tsugumi/api/` read-only.

## What the core evaluates

The result of a Card's `default.nix` may define only these attributes:

- `run`: a derivation containing `lib/main.so`, or a list of `{ label; run; }`;
- `views`: the Card's [views](#views);
- `exports`: values that downstream Cards may read.
- `api`: the optional code-generation derivation.

A language SDK's Nix builder only returns the derivation. There is no manifest. The build plan holds the run derivations, the ordered view list and, for remote or foreign execution, the core's worker.

## What the core needs from a language SDK

There is no language SDK metadata. The interface between the core and a language SDK is only:

1. **A Nix builder** that produces `lib/main.so` from the sources and the Card contract. The separate API builder publishes `share/tsugumi/api/`.
2. **[Card ABI](CardABI.md).**

What a language SDK might otherwise have described lives elsewhere:

- Highlighting by file extension is UI presentation.
- hostd formats JSON and Nix, chosen by file extension. Formatting Card source is the Agent's job, or the Card's `default.nix` can expose a formatter.
- `InputTypes` describes connected ports. A FlatBuffer port has a resolved schema. A Path port carries a file or directory and is declared as `string (tsugumi: "path")` in the port catalog. [Host presentation services](HostPresentation.md)
- The guide is ordinary documentation of the language SDK, in tsugumi-kit.
- The generated API under `share/tsugumi/api/` (for example `card.hh`) is the description of the types.

### Port schemas

The core generates a one-port input schema for each connected port. The SDK combines the port schemas into its typed input interface. Connected input fields are nullable. The Card expresses input requirements in its SDK's language, and the SDK reports `input_missing` with the port name when a required value is absent. The authored `_outputs.fbs` carries declared outputs and their `required` attributes.

When a Merge produces a union, each distinct FlatBuffer type has one member. Its box holds the original bytes in a `[ubyte]` field with `nested_flatbuffer` naming that type. The SDK decodes those bytes and exposes the member as its original type. The member names sent to the Canvas through RPC are presentation data.

## Templates

- Templates are independent of the language SDKs, and of the core and the UI.
- There are template stores at two levels:
  - the host, under Tsugumi's support folder;
  - the project: `templates/` in the project, versioned with it.
- When names collide, both are listed, each labelled with its store. Neither shadows the other.
- A template is a folder of Card definition folders. It may hold several Cards or a partial graph, plus an optional `layout.bin` fragment. Card IDs are remapped when it is copied.
- Users can add templates from a folder, or import them from a public HTTPS Git repository with a ref and a subdirectory.
- A "make template" skill may create templates at the project level.
- The core manages the list: it stores, lists and copies templates, and imports them from Git.
- The Agent or the user chooses a template. The core never chooses: there is no automatic selection and there are no per-language defaults.
- The Agent may list and copy templates. Adding or importing one needs a person's approval.
- Scaffolding a new Card is the Agent's job.
- The "New Card" menu offers Draft, Trigger, Input, Branch, Fork, Merge and Collect, and "From Template…" with Project and Host sections and nothing preselected. [Reuse Cards from a template](Usage.md#reuse-cards-from-a-template)
- No templates ship in the core. Example templates live in tsugumi-kit.

## Views

A Card declares its `views` next to `run`, `api` and `exports`:

```nix
{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/cpp> { inherit pkgs; };
  common = { inherit inputs params; outputs = ./_outputs.fbs; };
in
{
  run = sdk.cppCard (common // { src = ./src; });
  api = sdk.cppApi common;
  views = sdk.views ++ [
    { name = "notes"; type = "html"; blob = ./notes.html; }
  ];
}
```

- Each entry is `{ name; type; blob; }`. `type` is `html` or `dawn`. `blob` is one file in the Nix store: a self-contained HTML file for `html`, a Wasm binary for `dawn`.
- An SDK's own views are added explicitly with `sdk.views`. Nothing is merged implicitly. The C++ SDK's list holds html views for text, Markdown, tables, images, line charts (`LineChart`) and points (`Points`), and the Python SDK's list adds Plotly. Each view's bytes format is private to its SDK. The SDKs that had line charts and points before the Card ABI cutover keep them as html views; a `dawn` version comes later. An SDK with an empty list sends to views taken from another SDK's list, or the Card declares its own.
- The core reads the list and sends it to the UIs (`CardViewsUpdated`) before any run. An empty list clears the Card's views. The core does not interpret names, types or blob contents.
- The UIs bundle no views. Each UI shows a Card's declared views in the Card's view window, routed by `type`, and draws nothing on the Canvas.
- A Card sends to a view with the ABI's `send(view, bytes)` ([Card ABI](CardABI.md#effects-and-their-lifetimes)). The core relays `{view, bytes}` without resolving the name. The UI matches the name against the Card's declarations and shows an error for an unknown one.
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
