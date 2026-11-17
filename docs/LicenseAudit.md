# Licenses and distribution

[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) lists dependencies and their licenses. The app and CLI distributions include the full license texts; a dependency's license name alone does not replace its notices.

## Bundling license texts

[ui/macos/tools/bundle-licenses.sh](../ui/macos/tools/bundle-licenses.sh) copies license texts from the sources used for the build into `Contents/Resources/licenses/`. Missing required files stop the build.

- The native host Agent bundles OpenSSL 3's `libssl` and `libcrypto`; copy `LICENSE.txt` from the same nixpkgs OpenSSL source into `licenses/openssl/`. `build-host.sh` rewrites their loader paths before the app signs them.
- The native host Agent links libgit2 statically ([nix/libgit2-local.nix](../nix/libgit2-local.nix)), with the zlib, PCRE2, llhttp and xdiff sources it contains; copy `COPYING` from the same source into `licenses/libgit2/`.
- Keep Swift Markdown's `NOTICE.txt` and swift-cmark's complete `COPYING`, including the notices of embedded code.
- Keep Tree-sitter runtime, grammar and query notices. Sources are listed in [Grammars/README.md](../ui/macos/Sources/Grammars/README.md) and [TreeSitter/SOURCES.md](../ui/macos/Sources/App/Resources/TreeSitter/SOURCES.md).
- SwiftMath's MIT license covers its code, not its fonts. The app ships Latin Modern Math with the GUST Font License and its complete upstream package.
- The documentation site uses Yusei Magic, with its [SIL OFL text](site/assets/YuseiMagic-OFL.txt). The app's bundled documentation must include that text too.

## The guest image

The app release contains `tsugumi-vm.tar.gz`, `vm-manifest.json`, `SHA256SUMS`
and `vm-licenses.tar.gz`. The [image workflow](../.github/workflows/vm-image.yml)
builds them on aarch64-linux at the same immutable revision as the app. Mac
packaging consumes that run's artifact; it never fetches a separately published
boot disk. Both the app and CLI include the expanded inventory and license texts
under `licenses/vm-guest/`.

[vm-inventory.py](../vm/guest/tools/vm-inventory.py) reads the actual image's
`/store-paths`, assigns outputs to their derivations and adds the separately
shipped kernel. For every third-party package it records the license nixpkgs'
`meta.license` declares, the license files of its source and the upstream source
URLs and hashes. An output whose owner cannot be found, a package without a
declared license or without a license text in its source, or a kernel without
its embedded configuration stops artifact generation. Nixpkgs license
declarations are metadata, not a file-by-file audit.

Tsugumi's own outputs (tsugumid with the worker and the SDK, and the scripts
and files `vm-boot.nix` and `vm-system.nix` write) are one fixed entry: MIT,
the source being this repository at the release tag. GitHub's source archive of
the tag is that source. FlatBuffers' runtime headers, compiled into tsugumid,
keep their Apache-2.0 text.

No source archive of the dependencies is made, mirrored or attached: the
inventory points to their upstream sources, and the recipes are nixpkgs at the
revision in `flake.lock`, `flake.nix` and `vm/guest/` at the tag. The kernel's
base configuration is [kernel/base-6.12.28.config](../vm/guest/kernel/base-6.12.28.config)
with the changes [kernel.nix](../vm/guest/kernel.nix) applies; the resulting
configuration is `kernel-config` in the inventory. Its provenance is in
[kernel/SOURCES.md](../vm/guest/kernel/SOURCES.md).

## Web bundles

The `tsugumi` CLI packages the Web UI. `cli/package-macos.sh` collects its npm license texts with `ui/web/scripts/licenses.mjs --all`; preserve those notices when distributing `ui/web/dist` separately too.

`plotly.min.js` is distributed in upstream's minified form. Its overall license and some dependency notices are included, but not all dependency license texts. Vega's dependency license texts are collected from the lock file.

## Updating dependencies

Update [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) with dependency changes. Review transitive dependencies and font licenses, regenerate the VM inventory when changing the boot disk, and inspect the license directory in the distribution before publishing.
