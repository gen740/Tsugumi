# License audit

Audited on 2026-11-04 ([the source at 0ec6460](https://github.com/gen740/Tsugumi/tree/0ec64607f3a6348386694077e098f85a0f0ca73a), the SwiftPM checkout matching its lock file, the Web's lock file, Dawn's local build settings and the link rules it generated). On 2026-11-05, the ZIP and VM image of the published [v0.2.0](https://github.com/gen740/Tsugumi/releases/tag/v0.2.0) were unpacked and checked, and the remaining work was taken forward.

**Publishing Tsugumi's own code under MIT is compatible with the MIT / BSD licenses of Tree-sitter that were checked.** The full license texts of what goes into the app's distributions have been bundled since v0.3.0 ([what remains for distribution](#what-remains-for-distribution)). **The source corresponding to the GPL / LGPL software in the VM is placed on the boot disk's release** ([how the VM's corresponding source is provided](#how-the-vms-corresponding-source-is-provided)).

This document records what could be confirmed from the code, the build steps and the artifacts. It is not a legal conclusion.

## How strict to be

This is a project an individual publishes under MIT, distributing an ad hoc signed app on GitHub, so meeting what distribution requires comes first, without adding operational work.

| Treatment | What | Why |
| --- | --- | --- |
| Met | Full texts and notices of what goes into the app and the VM, and the corresponding source of the VM's GPL / LGPL software | The licenses state these as conditions of distribution. They are built into the publishing and build scripts, adding no manual work |
| Left to declarations and upstream documents | The license names of the VM's packages (nixpkgs' `meta.license`), and the license documents at the root of their sources | Auditing the sources of 93 packages file by file is not worth the cost. It is stated that these are declarations, not audits |
| Resolved by removing | 11 unused maths fonts | Not distributing them is surer and cheaper than checking them against upstream |
| Not pursued (with the reason kept) | The full texts of dependencies inside plotly.js's minified build, and the full texts for `web/dist`, which the author does not distribute | See "Plotly's dependencies" below and the table |
| Not checked | Drift between the notices' versions and the pins | It would add work at every dependency update. The full texts are copied from the checkout, so even with drift the texts distributed are right |

## Tree-sitter in detail

| What | What was checked | How the notices treat it |
| --- | --- | --- |
| Runtime | `tree-sitter` 0.25.10 is MIT; `SwiftTreeSitter` 0.25.0 is BSD-3-Clause | Listed separately |
| Nix / Python grammars | `src/` copied and used. Each directory already has the upstream LICENSE | Source and scope stated |
| C / C++ / JSON / Bash / Go / Rust / Julia / R grammars | Fetched with SwiftPM and linked. Each upstream LICENSE is MIT (Julia's copyright notice is Max Brunsfeld, GitHub; R's is tree-sitter-r authors). Julia's manifest does not include `scanner.c`, so only the scanner is copied into `Grammars/TreeSitterJuliaScanner/` (with its LICENSE) | Links to the LICENSE at the pinned revision, and the full license texts for copied queries |
| Highlight queries | `Resources/TreeSitter/SOURCES.md` gives the source per grammar. C++ also uses C's queries | From the description of sources to the full license texts |
| FBS grammar | `grammar.js` is this project's own; `src/parser.c` and the metadata are generated from it | Own grammar and generated files distinguished |
| `src/tree_sitter/` | External helper headers placed by the generator, also present in the folder of our own FBS grammar | `Grammars/LICENSE.tree-sitter` and its scope |
| `tree-sitter-swift` | For SwiftTreeSitter's `TestTreeSitterSwift`; not a dependency of the `SwiftTreeSitter` product the app uses | Not bundled with the app |

A list of license names alone, or a LICENSE in a checkout that exists only on the building machine, does not confirm that the text ships to recipients. For the original texts, see [MIT](https://opensource.org/license/mit) and [SwiftTreeSitter's LICENSE](https://github.com/ChimeHQ/SwiftTreeSitter/blob/08ef81eb8620617b55b08868126707ad72bf754f/LICENSE).

## Dependencies easy to overlook

- **Swift Markdown / swift-cmark**: Swift Markdown's `LICENSE.txt` is Apache-2.0 with the Swift runtime exception, and there is a `NOTICE.txt` too. swift-cmark's `COPYING` has, besides BSD-2-Clause, MIT notices for code derived from Houdini, GitHub, utf8proc and others. They must be kept in full, not omitted on the strength of one LICENSE name for the whole package.
- **SwiftMath and its fonts**: SwiftMath's code is MIT, but the fonts it bundles are not. See the table below.
- **hosq**: a library of the author's own, developed beforehand, is still a separate dependency. The LICENSE at the revision pinned in `flake.lock` is MIT. It is also stated in [How it was built](Philosophy.md#how-it-was-built).
- **libgit2**: linked by the core. The pinned [nixpkgs definition](https://github.com/NixOS/nixpkgs/blob/1c89083e451e891152365b799595cf7790a43325/pkgs/by-name/li/libgit2/package.nix) specifies 1.9.7. It is GPLv2 with a special linking exception, so reading it simply as "GPL, so the whole app is GPL" is inaccurate. Following the [upstream COPYING](https://github.com/libgit2/libgit2/blob/v1.9.7/COPYING), distributing the combined executable is distinguished from distributing the library itself.
- **The Web's transitive dependencies**: besides React and the like, `loose-envify` / `js-tokens` are in the lock file. `commander` is for the KaTeX CLI. What is in the lock file and what esbuild actually outputs are checked separately.
- **Dawn's transitive dependencies**: a monolithic static library for Metal goes into the renderer dylib. Followed in "What Dawn actually takes in" below.

Apache-2.0 dependencies also cover NOTICE files and the like ([Apache-2.0 §4](https://www.apache.org/licenses/LICENSE-2.0)).

## What Dawn actually takes in

For Dawn `1716e88`'s `libwebgpu_dawn.a` (652 objects), the 1547 non-SDK files among the compile inputs recorded by `ninja -t deps` were examined. The only thing from `third_party/` is Abseil; SPIRV-Tools and the like are not included.

| Included | Files | License | Full text bundled |
| --- | --- | --- | --- |
| Dawn / Tint themselves and generated code | 1244 | BSD-3-Clause | `dawn.LICENSE` |
| `gen/include/dawn/webgpu.h` (a generated file with the same notice as webgpu-headers) | 1 | BSD-3-Clause ("WebGPU native" developers) | `webgpu-headers.LICENSE` |
| `src/dawn/common/LinkedList.h` (a modified copy from Chromium) | 1 | BSD-style. The Chromium LICENSE it refers to is not in Dawn's source | `chromium.LICENSE` ([third-party/chromium/](../third-party/chromium/README.md)) |
| Abseil (`af5aede`, as imported by Chromium) | 301 | Apache-2.0, with no NOTICE | `abseil-cpp.LICENSE` |

Classified by the kind of copyright notice at the top of each file. The Apache-2.0 section of Dawn's `LICENSE` is for `generator/templates/art/*` and `tools/android/webgpu/*`, which are not included here. After changing Dawn's pin, build the renderer and review the breakdown of `third_party/` with `ninja -C build-renderer -t deps`.

## SwiftMath 1.7.3's fonts

The only font the app uses to draw maths is Latin Modern Math, the default of `MTMathUILabel` (`macOS/Sources` chooses no font; checked by [test_licenses.py](../tests/python/test_licenses.py)). Fonts are loaded when needed, and [SwiftMath's `MathFont.swift`](https://github.com/mgriebling/SwiftMath/blob/fa8244ed032f4a1ade4cb0571bf87d2f1a9fd2d7/Sources/SwiftMath/MathBundle/MathFont.swift) touches no other file. So `macOS/build-app.sh` removes the other 11 from the bundle. Distributions up to v0.2.0 contain all 12. That `MTMathUILabel` typesets maths with a bundle holding only Latin Modern was checked with a small executable that calls it the same way.

According to its upstream package, Latin Modern Math is under the GUST Font License, a form of the LPPL. Clause 2 of [LPPL 1.3c](https://www.latex-project.org/lppl/lppl-1-3c.txt) treats distributing only part of the Work as a modification, so it can be read that the `.otf` and license in SwiftMath's bundle are not enough. The bundle's `latinmodern-math.otf` matches the one in the upstream [latinmodern-math-1959.zip](https://www.gust.org.pl/projects/e-foundry/lm-math/download/latinmodern-math-1959.zip) by SHA-256, so the whole upstream package, under 1 MB, is bundled as it is from the pinned nixpkgs' `lmmath.src` into `licenses/fonts/latin-modern-math/`.

The `LICENSE` of [mathFonts.bundle](https://github.com/mgriebling/SwiftMath/tree/fa8244ed032f4a1ade4cb0571bf87d2f1a9fd2d7/Sources/SwiftMath/mathFonts.bundle) at the pinned revision is MathChat's MIT notice, not a blanket license for the 12 fonts (a list read from each font's `name` table is in the Git history). `GUST-FONT-LICENSE.txt` refers to the conditions of LPPL 1.3c or later. The conditions of XITS and KpMath Sans were not checked against upstream; as they are no longer distributed, it was decided not to. If another font is used in future, it will be checked and bundled then.

## The fonts of Help > Learn about Tsugumi

"Learn about Tsugumi" shows the documentation site's pages as they are, so its font is the same as in "The documentation site's font" below. `macOS/build-app.sh` copies it, with the full OFL text, into `Contents/Resources/explainer/site/assets/`.

## What remains for distribution

| Item | State | What was done, and where to check |
| --- | --- | --- |
| Bundling full license texts, NOTICEs and sources together in the app | **Resolved** (since v0.3.0) | [bundle-licenses.sh](../macOS/tools/bundle-licenses.sh) copies the documents of the checkout used for the build into `Contents/Resources/licenses/`. The build stops if any are missing |
| Keeping licenses in the Tsugumi source given to the VM | **Resolved** | Added to `macOS/Container/vm-source.txt` |
| Completing Dawn's list and full texts | **Resolved** | "What Dawn actually takes in" above |
| Completing the fonts' list and full texts | **Resolved** | "SwiftMath 1.7.3's fonts" above. Only Latin Modern Math and Yusei Magic are distributed |
| Establishing where the kernel config came from | **Resolved** | Confirmed byte for byte identical to the config embedded in Kata Containers 3.17.0's kernel ([SOURCES.md](../macOS/Container/kernel/SOURCES.md)) |
| The VM's contents and their full license texts | **Resolved** | [vm-inventory.py](../macOS/tools/vm-inventory.py) generates them from the image's `/store-paths` and the derivation graph and places them in `licenses.tar.gz` on the boot disk's release. The release build puts it into the app, and stops if it cannot get it |
| Providing the VM's corresponding source tied to the release | **Resolved** | "How the VM's corresponding source is provided" below |
| Including the full texts needed for the Web's distribution | **Not done** | The author does not distribute `web/dist`. It is not in the releases; each person builds it from a checkout (the full texts are in `web/node_modules/`). If it is ever distributed on its own, they will be collected from esbuild's metafile |
| How to treat releases already published | **Done in v0.3.0** (earlier versions not withdrawn) | "Published releases" below |
| Full texts of the dependencies inside plotly.js's bundle | **Not pursued (the author can change this)** | "Plotly's dependencies" below |

### How the VM's corresponding source is provided

The author's decision: **the whole set is placed on the boot disk's release ([gen740/tsugumi-vm](https://github.com/gen740/tsugumi-vm)).** For each disk, `macOS/tools/publish-vm.sh` places `sources.tar.gz` (754 MiB for the current disk) and `licenses.tar.gz` on the same release as the binaries. The app's `licenses/vm-boot/README.md` has that URL. The release build stops for a disk not yet published, so it is never distributed unpublished.

The set is made by [vm-source-bundle.py](../macOS/tools/vm-source-bundle.py) from the `inventory.json` that `vm-inventory.py` produced:

- each package's source, and the patches it fetches (126)
- nixpkgs at the pinned revision, with each package's configure, build and install steps and the patches inside nixpkgs
- the parts of Tsugumi that assemble the disk (`flake.nix`, `macOS/Container/` and so on), and the build instructions

Evaluating `vm-boot` from the unpacked set was confirmed to give the same store path as the published disk. Some of the archives (38) were also checked against the hashes in `inventory.json`. Whether rebuilding from scratch gives the same binaries has not been checked.

| Confirmed facts | |
| --- | --- |
| What is distributed | The contents of `Tsugumi.zip` and `Tsugumi.dmg`, and `vmlinux` and `rootfs.erofs` on the releases of gen740/tsugumi-vm |
| Contents | 103 store paths and the kernel: 93 packages. 37 of them include GPL or LGPL by nixpkgs' declarations (Linux, BusyBox, Bash, coreutils, glibc, Nix and others). Some are GPL-3.0, so [GPLv3 §6](https://www.gnu.org/licenses/gpl-3.0.html#section6) applies too |
| The kernel's generated config | Inside the binary (`CONFIG_IKCONFIG`) and as `kernel-config` in `licenses.tar.gz` |

Two points remain open. These are not legal conclusions.

- **Where:** [GPLv2 §3](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html#section3) treats distribution by download as distributing the source if the source can be had from "the same place". `vmlinux` on its own is on the same release, but `Tsugumi.zip` and `Tsugumi.dmg` are on the releases of gen740/Tsugumi, another repository. GPLv3 §6(d) states that another server is fine if directions are given; GPLv2 has no such statement. As it is the same owner on the same GitHub, with the URL inside the app, the author chose this form.
- **Keeping it:** while the binaries are distributed, their source is not deleted. When tidying the disk's releases, the app releases that carried that disk are checked too.

### Published releases

The author's decision: **fixed in v0.3.0; v0.1.0, v0.1.2 and v0.2.0 are not withdrawn.** Their ZIPs have no full license texts (unpacking v0.2.0 found only the three documents in SwiftMath's bundle). The three tags have the same nixpkgs revision and `vm-boot.nix`, and v0.2.0's VM was confirmed by unpacking to be the same disk (v0.1.x were not unpacked), so the published VM source also corresponds to these versions.

### Plotly's dependencies

`plotly.min.js` is the minified build upstream published, distributed as it is. The overall `LICENSE` and comments for some dependencies remain, but the full texts of the dependencies inside do not. For reference, resolving the dependencies of upstream's `plotly.js@3.6.0` gives 258 packages, 31 without a license file. What actually goes into the bundle is a subset of these, and without upstream's metafile it is not known which.

Rather than keep by hand a list that includes unrelated things, it is distributed in upstream's form. To be strict, the full texts could be generated from the resolved dependencies and bundled (about 270 KB, and 31 by hand), or plotly.js could be bundled here to get a metafile. Vega's dependencies are in the lock file, so their full texts are bundled.

### The VM is not exempt because "Nix fetches it"

The boot disk distributed contains GPL / LGPL binaries such as Linux (6.12.115, built with our own configuration in `kernel.nix`), Nix and Bash (the daemon and extra tools of `vm-system.nix`, and Cards' runtimes, are fetched after installation, so they are separate). Pinning nixpkgs in `flake.lock`, or showing how to fetch upstream sources, does not amount to distributing the source corresponding to these binaries ([GPLv2 §3](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html#section3) includes the build and install scripts), and dynamic linking is no reason to skip checking either ([LGPLv2.1 §§4–6](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)).

## The documentation site's font

Yusei Magic ([SIL OFL 1.1](site/assets/YuseiMagic-OFL.txt)), used for the handwriting in the figures on GitHub Pages, is placed in `docs/site/assets/` as a WOFF subset of the characters the figures use (the Latin letters, digits and punctuation), together with the full OFL text. With no Reserved Font Name, it may be modified under the same name. Pages load it as `Yusei Magic Subset`.

## Checking on updates

What stops by itself: missing full texts in the app, and boot disks not yet published (both stop the build). The boot disk's list and source are made by `macOS/tools/publish-vm.sh`, so nothing is generated by hand.

What people check: after updating dependencies, correct the versions in the notices. After changing Dawn's pin, review the breakdown of `third_party/`. Check the ZIP to be published one last time.
