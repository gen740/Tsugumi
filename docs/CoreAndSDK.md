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

- `params` is the evaluated literal attrset from [params.nix](CardGuide.md#parameters), or `{ }` when the Card has no params.nix. The SDK embeds it at build time.
- `inputs` contains Nix exports from the guaranteed upstream Cards, by Card ID. In `A → B → C`, C gets both `inputs.A` and `inputs.B`. TODO(open): Exports through graph boundaries.

Import a language builder from `<tsugumi-kit/sdk/cpp>` or another SDK entry point with `{ inherit pkgs; }`. Pass evaluated `params`, the generated `_inputs.fbs` as the builder's `inputs`, and the authored `_outputs.fbs` as `outputs`. Run and API builders share code generation. See the [temperature example](CardGuide.md#the-files) and the [kit builder contract](https://github.com/gen740/tsugumi-kit/blob/main/docs/sdk-contract.md).

### The generated API (`share/tsugumi/api/`)

The Card's top-level `api` attribute is a code-generation derivation. Its output contains the generated API and a `README.md` listing the types under `share/tsugumi/api/`. The run and API builders share code generation. Normal Card builds leave `api` unevaluated.

### Evaluating a Card expression

The [generic evaluation tool](features.md#evaluating-a-card-expression) runs a Nix expression in the saved Card's directory on the local system. It can return a JSON value or build one derivation and return its regular output files. **Build SDK** in the Mac and Web Card editors builds `(import ./. { inherit params inputs; }).api` with that tool and shows `share/tsugumi/api/` read-only.

## What the core evaluates

The result of a Card's `default.nix` may define only these attributes:

- `run`: a derivation containing `lib/main.so` for Code Cards or built port values for Input Cards, or a list of `{ label; run; }`;
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
- `InputTypes` describes the connected ports. The generated API files (for example `card.hh`) show the SDK's interface. See [Host presentation services](HostPresentation.md).

### Port schemas

The core generates one `_inputs.fbs` per Code or Input Card. The schema describes the connected ports and contains namespaced copies of their upstream types. Cards with no connected inputs get an empty `Inputs` table. The generated `tsugumi_schema` attributes let the SDK select a candidate by the ID in the port envelope. The file is versioned and refreshed when wiring or upstream schemas change.

Merge forwards each envelope unchanged. A Merge-fed port with one distinct schema has that type. With several distinct schemas, its generated input schema declares a union. The UI displays the candidate type names as `A | B`. Collect feeds a fixed `Collected` table with opaque members.

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
- The "New Card" menu offers Draft, Trigger, Input, Merge and Collect, and "From Template…" with Project and Host sections and nothing preselected. [Reuse Cards from a template](Usage.md#reuse-cards-from-a-template)
- No templates ship in the core. Example templates live in tsugumi-kit.

## Views

A Card declares its `views` next to `run`, `api` and `exports`:

```nix
{ params, inputs }:
let
  pkgs = import <nixpkgs> { };
  sdk = import <tsugumi-kit/sdk/cpp> { inherit pkgs; };
  common = {
    inherit params;
    inputs = ./_inputs.fbs;
    outputs = ./_outputs.fbs;
  };
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
- Include SDK-provided views explicitly with `sdk.views`. LineChart and Points are HTML views in the SDK. Custom HTML can include its own JS and Wasm.
- The Card calls `view_open` by name to create a window. Each open creates a session and an opener route. A Card that receives a session from a port can send to that window and register its own route to receive responses.
- PANEL opens in the project window on macOS and over the canvas on the Web. WINDOW opens a separate window and accepts an optional initial content size. FULLSCREEN opens a native fullscreen window on macOS and a browser child window with a fullscreen button on the Web. A packed app presents PANEL in its main window.
- Messages travel to one session or one route through the worker, core and UI. The relay uses [ABI backpressure](CardABI.md#display-messages). A window retains its last frame after its routes go away. Closing the window gives every live route a close event.
- View traffic is live. Use output ports and artifacts for results that people will inspect later. [Run history](RunHistory.md)

### Passing a window between Cards

A title Card can open a view and pass its session in a port alongside the
initial state. The play Card registers its own route on that session and
sends the route to the view in a message. The view can direct input to the
play Card using that route. Passing the same session to a shop Card lets
it register another route and render into the same window.

### html views

The UI loads the file in an isolated frame without network access. Use
`tsugumi.onmessage = (bytes) => { /* draw */ }` to receive Card messages.
Use `tsugumi.send(bytes, route)` to send to a specific route, where `route` is
a 16-byte `Uint8Array`. Omitting it sends to the opener's route. The view
draws its empty state and handles resize. SDK views reassemble their own chunks.

### dawn views

A dawn view is a wasm32 module that draws to one WebGPU surface. Its contract is the
[view header](../protocol/include/tsugumi/tsugumi_view.h) (dawn view ABI 0):

- **Imports:** only the WebGPU functions listed in `TSUGUMI_VIEW_WEBGPU_IMPORTS`
  (49 functions of `webgpu.h` from webgpu-headers at the revision the header pins),
  plus `tsugumi_view_send` and `tsugumi_view_read`. No WASI, files or clock. WebGPU
  handles are host-owned ids. No imported function writes into the module's memory
  or takes a callback; the only chained struct is WGSL source on a shader module.
  Built with clang for wasm32, the header turns a function outside the list into an
  undefined symbol at link time.
- **Exports:** `memory`, `tsugumi_view_abi`, `tsugumi_view_start`, `tsugumi_view_resize`
  and `tsugumi_view_draw` are required; `tsugumi_view_message`, `tsugumi_view_pointer`
  and `tsugumi_view_key` are optional.
- **Calls:** the host checks the imports and exports, calls `abi` (it must return 0),
  then `start` and one `draw`. It draws again on the next display frame only when
  `draw` returns bit 0, and otherwise only after an event (message, resize, pointer,
  key). `read` copies the current message only inside `message(size)`.
  `tsugumi_view_send(route, data, size)` takes a 16-byte route. An all-zero route
  addresses the opener. Sending is nonblocking. The host rejects routes from
  other windows and drops traffic for dead routes. A negative callback return
  or a trap drops the instance and shows the error.
  All calls come from one thread; there is one instance per view.

Both UIs translate the module's 32-bit descriptors with code generated at build time
from webgpu-headers' `webgpu.yml` by one generator ([ui/shared/dawn](../ui/shared/dawn/README.md)):

- **Mac app:** native Dawn and wasmtime inside the app process, drawing into a
  `CAMetalLayer` view in the SwiftUI canvas, paced by `CADisplayLink`.
- **Web UI:** the same module in the page, with the imports implemented on
  `navigator.gpu` and paced by `requestAnimationFrame`.

The kit's `sdk.dawnView { name; src; }` builds a view from C sources (freestanding,
no libc), and its `dawn-view` template is a minimal example. The `point-cloud` demo
sends a point cloud to a dawn view.

TODO(open): PNG snapshots of dawn views for the Agent.

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
