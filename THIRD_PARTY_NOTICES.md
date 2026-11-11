# Third-party notices

Tsugumi's own code is under the [MIT License](LICENSE). Existing libraries,
copied code, fonts and external data retain their own licenses, including
the [hosq](https://github.com/CLI20-dev/hosq) library.

The app carries these notices and the full license texts in
`Contents/Resources/licenses/` ([bundle-licenses.sh](macOS/tools/bundle-licenses.sh)).
The corresponding source of the VM's boot disk is in that disk's release of
[gen740/tsugumi-vm](https://github.com/gen740/tsugumi-vm); see
[Licenses and distribution](docs/LicenseAudit.md). Lock files and build recipes
are authoritative when dependencies change.

## Tree-sitter: copied sources and linked packages

The editor uses the runtime, language grammars and highlight queries.
Their MIT licenses permit use alongside Tsugumi's own MIT code, while
requiring preservation of their copyright and permission notices.
SwiftTreeSitter has its own BSD license.

| Component | Version / license text | Use |
| --- | --- | --- |
| Tree-sitter runtime | [0.25.10, MIT](https://github.com/tree-sitter/tree-sitter/blob/da6fe9beb4f7f67beb75914ca8e0d48ae48d6406/LICENSE) | Runtime linked through SwiftTreeSitter; generator-supplied support headers also remain external code |
| SwiftTreeSitter | [0.25.0, BSD-3-Clause](https://github.com/ChimeHQ/SwiftTreeSitter/blob/08ef81eb8620617b55b08868126707ad72bf754f/LICENSE) | Swift wrapper |
| tree-sitter-c | [0.23.6, MIT](https://github.com/tree-sitter/tree-sitter-c/blob/ec69f91b23dd8630d68710b911d77127146ff7ef/LICENSE) | Linked grammar and copied highlight query |
| tree-sitter-cpp | [0.23.4, MIT](https://github.com/tree-sitter/tree-sitter-cpp/blob/f41e1a044c8a84ea9fa8577fdd2eab92ec96de02/LICENSE) | Linked grammar and copied highlight query (also uses the C query) |
| tree-sitter-json | [0.24.8, MIT](https://github.com/tree-sitter/tree-sitter-json/blob/ee35a6ebefcef0c5c416c0d1ccec7370cfca5a24/LICENSE) | Linked grammar and copied highlight query |
| tree-sitter-bash | [0.25.1, MIT](https://github.com/tree-sitter/tree-sitter-bash/blob/a06c2e4415e9bc0346c6b86d401879ffb44058f7/LICENSE) | Linked grammar and copied highlight query |
| tree-sitter-go | [0.25.0, MIT](https://github.com/tree-sitter/tree-sitter-go/blob/1547678a9da59885853f5f5cc8a99cc203fa2e2c/LICENSE) | Linked grammar and copied highlight query |
| tree-sitter-rust | [0.24.2, MIT](https://github.com/tree-sitter/tree-sitter-rust/blob/77a3747266f4d621d0757825e6b11edcbf991ca5/LICENSE) | Linked grammar and copied highlight query |
| tree-sitter-julia | [0.25.0, MIT](https://github.com/tree-sitter/tree-sitter-julia/blob/e0f9dcd180fdcfcfa8d79a3531e11d99e79321d3/LICENSE) | Linked grammar and copied highlight query; its external scanner (`src/scanner.c`) is copied into `macOS/Sources/Grammars/TreeSitterJuliaScanner/`, with its [LICENSE](macOS/Sources/Grammars/TreeSitterJuliaScanner/LICENSE), because the package's manifest leaves it out |
| tree-sitter-r | [1.3.0, MIT](https://github.com/r-lib/tree-sitter-r/blob/346d3707b8c9301f1051e8f6e32666e67529f7d2/LICENSE) | Linked grammar and copied highlight query |
| tree-sitter-nix | e49c2f317e896eb08967717e758a46d6eea038b2, MIT | Copied into `macOS/Sources/Grammars/TreeSitterNix/`, with its [LICENSE](macOS/Sources/Grammars/TreeSitterNix/LICENSE), and copied highlight query |
| tree-sitter-python | 0.25.0, MIT | Copied into `macOS/Sources/Grammars/TreeSitterPython/`, with its [LICENSE](macOS/Sources/Grammars/TreeSitterPython/LICENSE), and copied highlight query |

`TreeSitterFBS/grammar.js`, its generated `src/parser.c`, grammar metadata
and `fbs.scm` are Tsugumi-specific. This does **not** make the accompanying
`src/tree_sitter/` support headers Tsugumi-authored code. Their notice is in
[Grammars/LICENSE.tree-sitter](macOS/Sources/Grammars/LICENSE.tree-sitter).
See the [grammar notes](macOS/Sources/Grammars/README.md) and
[query sources](macOS/Sources/App/Resources/TreeSitter/SOURCES.md).
The copied queries' upstream license texts are retained in
[`third-party/tree-sitter/`](third-party/tree-sitter/README.md).

`tree-sitter-swift` is used by SwiftTreeSitter's **test target**, not by the
`SwiftTreeSitter` product that Tsugumi links. It is not an app dependency
in the `macOS/Package.swift` / `Package.resolved` graph.

## Other macOS app dependencies

| Component | Pinned version / license text | License and scope |
| --- | --- | --- |
| FlatBuffers | [25.12.19](https://github.com/google/flatbuffers/blob/7e163021e59cca4f8e1e35a7c828b5c6b7915953/LICENSE) | Apache-2.0; Swift runtime (also used by core / SDKs) |
| SwiftTerm | [1.11.2](https://github.com/migueldeicaza/SwiftTerm/blob/b1262db5b6bea699a8260a8c66999436c508ca56/LICENSE) | MIT; includes xterm.js / SourceLair / Christopher Jeffrey attributions |
| Swift Markdown | [0.7.3 LICENSE](https://github.com/swiftlang/swift-markdown/blob/7d9a5ce307528578dfa777d505496bd5f544ad94/LICENSE.txt), [NOTICE](https://github.com/swiftlang/swift-markdown/blob/7d9a5ce307528578dfa777d505496bd5f544ad94/NOTICE.txt) | Apache-2.0 with Swift runtime exception; retain NOTICE too |
| swift-cmark | [0.9.0 COPYING](https://github.com/swiftlang/swift-cmark/blob/08ddb528923cc1a6527e02b7a1aee9e516ca749a/COPYING) | BSD-2-Clause and embedded MIT notices for derived code; test/spec material has separate terms in COPYING |
| SwiftMath | [1.7.3](https://github.com/mgriebling/SwiftMath/blob/fa8244ed032f4a1ade4cb0571bf87d2f1a9fd2d7/LICENSE) | MIT for code. Of its 12 OTF fonts, which have **separate** terms, the app ships only Latin Modern Math 1.959 (GUST Font License, an instance of the LPPL), the one it draws with, and with it the font's complete upstream package; see [LicenseAudit.md](docs/LicenseAudit.md) |
| Dawn / Tint | [v20261023.194543](https://github.com/google/dawn/blob/1716e88cc9e58398fab5d4473f7f2e858667b677/LICENSE) | BSD-3-Clause; statically incorporated into `libtsugumi_renderer.dylib` |

Dawn's Metal build also incorporates third-party code. Following the compile
inputs of the static library's 652 objects (`ninja -t deps`), what comes from
outside Dawn and Tint is Abseil (Apache-2.0; `af5aede` of Chromium's copy), the
notice of webgpu-headers in the generated `webgpu.h` (BSD-3-Clause) and
`src/dawn/common/LinkedList.h`, a modified copy of Chromium's
[linked_list.h](third-party/chromium/README.md) (BSD-style). Nothing else under
Dawn's `third_party/` is compiled, including SPIRV-Tools. See
[LicenseAudit.md](docs/LicenseAudit.md) and `ui/renderer/CMakeLists.txt`.

Swift Argument Parser 1.8.2 (Apache-2.0 with Swift runtime exception) is
resolved for dependency command-line / example targets; it is not a direct
Tsugumi app product dependency. A lock file entry alone is not proof that
its code is present in the final executable. Apple's system frameworks are
used from the operating system, not copied into the app.

## The `tsugumi` command

The Mac's `tsugumi-macos-arm64.tar.gz` also contains the official Node.js build
(version and checksum in `cli/node-version.env`; MIT and the notices in its
`LICENSE`, included as `licenses/node.LICENSE`), the Web UI below with the license files of its
npm packages (`licenses/npm`), the boot image below, and FlatBuffers in
`tsugumi-vmhost`. On Linux and in WSL on Windows, Nix builds the same from source (`nix/cli.nix`).

## Web UI

`web/package-lock.json` pins this runtime package tree. esbuild bundles
only code reachable from the server and browser entry points.

| Component | Version | License |
| --- | --- | --- |
| React, React DOM | 18.3.1 | MIT |
| scheduler | 0.23.2 | MIT |
| loose-envify, js-tokens | 1.4.0, 4.0.0 | MIT; React dependency tree |
| marked | 18.1.1 | MIT |
| KaTeX | 0.19.0 | MIT; Tsugumi requests MathML output and does not copy KaTeX fonts |
| commander | 15.0.0 | MIT; KaTeX CLI dependency, not an app import |
| FlatBuffers | 25.9.23 | Apache-2.0 |
| xterm.js (`@xterm/xterm`, `@xterm/addon-fit`) | 5.5.0, 0.10.0 | MIT |
| node-pty | 1.1.0 | MIT; a native module, loaded from `web/node_modules` and not bundled |

### Plot viewer (web UI and macOS app)

Plotly figures and Altair (Vega-Lite) charts of Cards are drawn by the
module viewer (`ui/modules`) with these minified browser builds, pinned in
`web/package-lock.json`. The web build serves them from `/modules/` and
`macOS/build-app.sh` copies them to `Contents/Resources/modules/`, each with
its license file (`LICENSE.<package>.txt`). The licenses of the packages the
Vega builds were made from are in the app's `licenses/plot-viewer/`.

| Component | Version | License |
| --- | --- | --- |
| plotly.js (`plotly.js-dist-min`) | 3.6.0 | MIT; the bundle, shipped as upstream publishes it, includes dependencies (d3 modules and others). Some of their notices are in its comments; the rest are not reproduced here, see [LicenseAudit.md](docs/LicenseAudit.md) |
| Vega (`vega`) | 6.4.0 | BSD-3-Clause; the build includes d3 modules (ISC, BSD-3-Clause) |
| Vega-Lite (`vega-lite`) | 6.4.3 | BSD-3-Clause |
| vega-embed | 7.3.0 | BSD-3-Clause |

The Python packages a Card asks for (`matplotlib`, `plotly`, `altair`, …)
come from nixpkgs when the Card is built; a Card that uses matplotlib also
gets the IPAex fonts (IPA Font License v1.0) as its fallback for Japanese.

`web/dist/` is built from a checkout, not distributed by this project; each
installed package in `web/node_modules/` contains its license text. esbuild,
TypeScript and type declarations are build-time dependencies.

## Core daemon and execution environment

The shipped macOS app contains Tsugumi source and a **boot VM image**.
`macOS/Container/vm-system.nix` builds the daemon and project tools in the
user's VM on first boot. These are different distribution boundaries.

| Component used by core | License / source |
| --- | --- |
| hosq, `1eb894224b0feffbb5b81d075d3e2c76160f99a1` | [MIT](https://github.com/CLI20-dev/hosq/blob/1eb894224b0feffbb5b81d075d3e2c76160f99a1/LICENSE); pre-existing author library, included as headers |
| Nix libraries (`nix-expr`, `nix-store`, `nix-fetchers`, `nix-flake`, `nix-util`) | LGPL-2.1-or-later; dynamically linked by the Nix build |
| Boehm–Demers–Weiser GC | Its MIT-style license; transitive Nix dependency |
| FlatBuffers | Apache-2.0 |
| SQLite | Public domain |
| libgit2 1.9.7 | GPL-2.0-only with its linking exception; see [COPYING](https://github.com/libgit2/libgit2/blob/v1.9.7/COPYING). The exception permits combinations with differently licensed programs; libgit2 itself retains its own license. |

The core's full runtime closure also includes transitive libraries; this
table is not a closure-wide license manifest. Package versions and recipes
are fixed by [flake.lock](flake.lock) and [nix/core.nix](nix/core.nix).

### Boot image redistributed with the app

[`vm-boot.nix`](macOS/Container/vm-boot.nix) includes Nix, CA certificates,
Bash, coreutils, findutils, util-linux, e2fsprogs, BusyBox, gawk, and their
runtime closure. [`kernel.nix`](macOS/Container/kernel.nix) builds Linux
6.12.115 (GPL-2.0-only), with a Tsugumi configuration derived from
[that of Kata Containers 3.17.0](macOS/Container/kernel/SOURCES.md). This is not
simply an unchanged nixpkgs kernel.

The boot closure contains GPL / LGPL software as well as permissively
licensed dependencies. `licenses.tar.gz` in the disk's release of
[gen740/tsugumi-vm](https://github.com/gen740/tsugumi-vm), which the app also
carries in `licenses/vm-boot/`, lists what the image really contains, with each
package's license as nixpkgs declares it, its sources and patches, the license
files of those sources and the kernel's configuration. The corresponding source
is `sources.tar.gz` in the same release. See
[how it is provided](docs/LicenseAudit.md#how-the-vms-corresponding-source-is-provided).
See [GPLv2 §3](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html#section3)
and [LGPLv2.1 §§4–6](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html).

### Downloaded / built after installation

The VM system additionally obtains tsugumid, bubblewrap, tini, shell tools
and formatters (astyle, Ruff, nixfmt, shfmt) through Nix. Card compilers,
Python and scientific libraries are obtained for each Card environment.
These are not all preinstalled in the distributed boot image. If a release
starts bundling their binaries or a populated Nix store, its redistribution
inventory and obligations must expand accordingly. The same applies when
distributing a standalone daemon or a different container image.

## Documentation site

The pages under `docs/` for GitHub Pages (`docs/site/`) draw the sketches' handwriting with a font and show an image. The app's Help > Learn about Tsugumi shows the same pages: `build-app.sh` copies them, with these files and the font's license text, to `Contents/Resources/explainer/`.

| Component | Version / license text | Use |
| --- | --- | --- |
| Yusei Magic | [google/fonts `f58dfb9`](https://github.com/google/fonts/tree/f58dfb9001f31a5eb15b4960b9548b8fd0fa6168/ofl/yuseimagic), [SIL OFL 1.1](docs/site/assets/YuseiMagic-OFL.txt) (Copyright 2020 The Yusei Magic Project Authors; no Reserved Font Name) | `docs/site/assets/YuseiMagic-subset.woff`, subset to the characters of the figures with fonttools |
| Cas A RGB image | Output of the Cas A demo's `rgb` Card (Chandra ObsID 25628) | `docs/site/assets/casa-rgb-25628.png`, the result in a figure; derived from Chandra observation data (see External data below) |

## External software

The Cas A demo builds CIAO (Chandra X-ray Center, GPL version 3 or later) from
source on the first run, with CCfits and libwcs from WCSTools, by the Nix
expressions of https://github.com/gen740/ciao, which the demo reads at a fixed
commit. Their sources are downloaded from upstream by
hash and are not included in this repository or in the app.

## External data

The Cas A demo fetches Chandra observation data from HEASARC on demand.
Its source and hash are recorded in the demo definition. Those observations
are not Tsugumi-authored data and are not relicensed under Tsugumi's MIT
license; source-specific data use and acknowledgement requirements remain
applicable when distributing results or datasets.
