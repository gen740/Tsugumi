# Card environments and Nix evaluation

## An environment per Card

A Code Card's `default.nix` receives `{ params, inputs }`. It imports its dependencies by name and returns a build specification. The [builder interface](CoreAndSDK.md#what-the-core-evaluates) lists its attributes. See the [temperature Card](../CardGuide.md#the-files) for a complete definition, and the [builder interface](CoreAndSDK.md#the-builder-interface) for the upstream `inputs` bindings.

- `builtins.currentSystem` is the system that runs the Card, locally or on a remote worker. `import <nixpkgs> { }` selects that system's packages. Evaluator state and caches are keyed by system.
- The core resolves the pinned sources and verifies their NAR hashes before evaluation. The restricted evaluator performs no fetching and disallows Import From Derivation (IFD). Fixed-output derivations such as `pkgs.fetchurl` can fetch by hash during a build.
- When a run starts, local Code Cards are evaluated and handed to `nix-daemon` without waiting for their data inputs. A remote worker builds its Card when it receives the invocation. Nix reuses available store paths, including those from a signed [binary cache](../BinaryCaches.md).

## Project Nix search path

`nixPath` in `tsugumi.json` lists the sources a project's Cards can import. Each entry has a `name` and a `url`.

```json
"nixPath": [
  {"name": "nixpkgs", "url": "github:NixOS/nixpkgs/nixos-unstable"},
  {"name": "tsugumi-kit", "url": "github:gen740/tsugumi-kit/main"}
]
```

Names are unique and match `[A-Za-z0-9][A-Za-z0-9._-]*`. Use different names for different versions, for example `nixpkgs` and `nixpkgs-2505`. URLs use source references accepted by Nix, such as Git repositories or HTTPS tarballs. Flake-only sources are unsupported. A source must expose Nix files that a Card can read with `import <name/path>`.

Open **Project settings → Name and Environment → Nix search path** and add the source. **Update** locks one name and leaves the other entries in place. **Update all** locks the current declarations together and drops entries for removed names. Select the owning project when working on an external. From the CLI:

```sh
tsugumi lock update nixpkgs --project /path/to/project
tsugumi lock update --project /path/to/project
```

Use `--project-path externals/id` to select a child project. The Agent uses `nix_path_lock_update`; the lock is in the project, so an update can be undone from version history. The `tsugumi.lock` root contains an `entries` list. Each entry stores `name`, the declared `url`, the resolved `rev` when supplied, and the verifying `narHash`. Updating adds missing entries, removes undeclared ones and resolves changed URLs. It keeps an existing pin for an unchanged declaration. Evaluation requires each declared name's locked URL to match its declaration. [Example lock file](../CardGuide.md#the-files)

Imports such as `<nixpkgs>` and `<tsugumi-kit/sdk/cpp>` resolve from Card definition files against their own project's names. An undeclared name raises a catchable evaluation error and lists the declared names. Host `NIX_PATH` is ignored. A project under `externals/<id>/` carries its own `tsugumi.json` and `tsugumi.lock` and resolves names independently of its parent.

After copying a Card to another project, add any required `<name>` missing from the destination's `nixPath` and update its lock.

## Evaluating a Card expression

The Agent's `evaluate_nix` tool evaluates a short expression on the local system, relative to a saved Card's definition directory. It has the project's locked search path and the Card build's `params` and `inputs`.

Evaluation returns bounded JSON. A derivation evaluates to its output path. With `build: true`, the expression must yield a derivation. The tool builds it in the Nix sandbox and returns regular files as relative paths and bytes, capped at 2,000 entries and 8 MiB of paths and file bytes. A failed build returns the end of its log, up to 256 KiB. **Build SDK** evaluates `(import ./. { inherit params inputs; }).api` with `build: true` and opens `share/tsugumi/api/` read-only.

```sh
tsugumi evaluate nix --project /path/to/project --card '{"segments":["convert"]}' \
  --expression '(import ./. { inherit params inputs; }).api' --build
```

## Sharing Nix exports through connections

The [project file reference](../Reference.md#nix-inputs-across-cards) defines
how upstream exports reach a Card and how built-in Cards propagate them.

## Keeping build results

Use [Build Cache settings](../Usage.md#manage-build-and-scratch-space) to inspect and explicitly clear build registrations and store space. [Binary caches](../BinaryCaches.md) explains shared caches.

## Evaluation reuse

The restricted evaluator reads participating Card definitions in place.
Before and after evaluation or reuse, the core checks their live files against
the confirmed document. A difference requires Reload before evaluation can
proceed. The accessor exposes only the files in the evaluation key and
supplies the confirmed generated input schema at `_inputs.fbs` in memory.
The root `_card.json`, `README.md`, and `_comments.json` stay outside the
source tree presented to Nix. The evaluator also permits reads from the
resolved locked sources.

The core can reuse a successful evaluation for the same Card, target system
and evaluation mode. The key includes evaluated source pins, definition and
schema digests, and the upstream Nix export graph. Layout, comments and display
names are outside this key. The evaluator revalidates observed filesystem
reads, including file contents, symlinks, directory membership and negative
lookups. It hashes observed files by streaming their contents. Filesystem
metadata gates repeated hashing. If an observed input changes, evaluation
starts in a new evaluator state so Nix's cached imports do not mask the edit.

The in-memory cache holds up to 256 entries. Reuse requires valid store paths
and observations within the evaluator's tracking bounds. The invocation and
its result recording still run under the ordinary execution rules.
