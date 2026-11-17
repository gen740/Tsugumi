# Licenses and distribution

[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) lists dependencies and their licenses. The app and CLI distributions include the full license texts; a dependency's license name alone does not replace its notices.

## Bundling license texts

[macOS/tools/bundle-licenses.sh](../macOS/tools/bundle-licenses.sh) copies license texts from the sources used for the build into `Contents/Resources/licenses/`. Missing required files stop the build.

- The native host Agent bundles OpenSSL 3's `libssl` and `libcrypto`; copy `LICENSE.txt` from the same nixpkgs OpenSSL source into `licenses/openssl/`. `build-host.sh` rewrites their loader paths before the app signs them.
- The native host Agent links libgit2 statically ([nix/libgit2-local.nix](../nix/libgit2-local.nix)), with the zlib, PCRE2, llhttp and xdiff sources it contains; copy `COPYING` from the same source into `licenses/libgit2/`.
- Keep Swift Markdown's `NOTICE.txt` and swift-cmark's complete `COPYING`, including the notices of embedded code.
- Keep Tree-sitter runtime, grammar and query notices. Sources are listed in [Grammars/README.md](../macOS/Sources/Grammars/README.md) and [TreeSitter/SOURCES.md](../macOS/Sources/App/Resources/TreeSitter/SOURCES.md).
- SwiftMath's MIT license covers its code, not its fonts. The app ships Latin Modern Math with the GUST Font License and its complete upstream package.
- The documentation site uses Yusei Magic, with its [SIL OFL text](site/assets/YuseiMagic-OFL.txt). The app's bundled documentation must include that text too.

## How the VM's corresponding source is provided

The app release contains `tsugumi-vm.tar.gz`, `vm-manifest.json`, `SHA256SUMS`,
`vm-licenses.tar.gz` and `vm-sources.tar.gz` (or numbered source parts). The
[image workflow](../.github/workflows/vm-image.yml) builds them on aarch64-linux
at the same immutable revision as the app. Mac packaging consumes that run's
artifact; it never fetches a separately published boot disk. Both the app and
CLI include the expanded inventory and license texts under `licenses/vm-guest/`.

[vm-inventory.py](../macOS/tools/vm-inventory.py) reads the actual image's
`/store-paths`, assigns outputs to their derivations, adds the separately shipped
kernel and records local scripts, Tsugumi and the embedded hosq input explicitly.
Unknown output owners, absent license declarations/texts or kernel configuration
stop artifact generation. Nixpkgs license declarations are metadata, not a
file-by-file audit.

[vm-source-bundle.py](../macOS/tools/vm-source-bundle.py) exports the complete
recursive derivation/source closure: local native filesets, all pinned flake
inputs, fetched sources and patches, nixpkgs-resident patches/configuration,
and the staged Tsugumi checkout. Dirty development builds archive the source
Nix actually evaluated. No manually maintained runtime source list is involved.
The archive includes `sources.nar`, `source-paths.json`, the inventory and rebuild
instructions. In a fresh aarch64-linux Nix store, extract it, run
`nix-store --import < sources.nar`, then `nix-store --realise <image_derivation>`
from `licenses/inventory.json`. Compilers and other ordinary build outputs can
be rebuilt or substituted; all source inputs are in the export. No original
checkout is needed. Verify source-part checksums, then reassemble with
`cat vm-sources.tar.gz.part* > vm-sources.tar.gz` if the source asset exceeded
the release's per-asset limit. The image itself remains one payload.

Keep these release assets available while distributing the binaries. Historic
VM releases and their sources are retained. The kernel's provenance is in
[kernel/SOURCES.md](../macOS/Container/kernel/SOURCES.md). Full source-rebuild
and actual VM checks must be performed on their corresponding Linux/Mac hosts;
a successful metadata check alone does not establish those results.

## Web bundles

The `tsugumi` CLI packages the Web UI. `cli/package-macos.sh` collects its npm license texts with `web/scripts/licenses.mjs --all`; preserve those notices when distributing `web/dist` separately too.

`plotly.min.js` is distributed in upstream's minified form. Its overall license and some dependency notices are included, but not all dependency license texts. Vega's dependency license texts are collected from the lock file.

## Updating dependencies

Update [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) with dependency changes. Review transitive dependencies and font licenses, regenerate the VM inventory and source bundle when changing the boot disk, and inspect the license directory in the distribution before publishing.
