# Licenses and distribution

[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) lists dependencies and their licenses. The app and CLI distributions include the full license texts; a dependency's license name alone does not replace its notices.

## Bundling license texts

[macOS/tools/bundle-licenses.sh](../macOS/tools/bundle-licenses.sh) copies license texts from the sources used for the build into `Contents/Resources/licenses/`. Missing required files stop the build. Keep license files in the source list supplied to the VM, [vm-source.txt](../macOS/Container/vm-source.txt).

- Keep Swift Markdown's `NOTICE.txt` and swift-cmark's complete `COPYING`, including the notices of embedded code.
- Keep Tree-sitter runtime, grammar and query notices. Sources are listed in [Grammars/README.md](../macOS/Sources/Grammars/README.md) and [TreeSitter/SOURCES.md](../macOS/Sources/App/Resources/TreeSitter/SOURCES.md).
- Dawn's Metal renderer includes Dawn/Tint, webgpu-headers, Chromium's linked-list code and Abseil. Bundle their license texts. After changing Dawn's pin, inspect the build inputs with `ninja -C build-renderer -t deps` and update the bundled notices for any dependency changes.
- SwiftMath's MIT license covers its code, not its fonts. The app ships Latin Modern Math with the GUST Font License and its complete upstream package.
- The documentation site uses Yusei Magic, with its [SIL OFL text](site/assets/YuseiMagic-OFL.txt). The app's bundled documentation must include that text too.

## How the VM's corresponding source is provided

[macOS/tools/publish-vm.sh](../macOS/tools/publish-vm.sh) publishes `sources.tar.gz` and `licenses.tar.gz` alongside the boot disk on [gen740/tsugumi-vm](https://github.com/gen740/tsugumi-vm). The app's `licenses/vm-boot/README.md` points to that release. Release builds require a published boot disk and its license archive.

[vm-inventory.py](../macOS/tools/vm-inventory.py) derives the package inventory from the image's `/store-paths` and derivation graph. [vm-source-bundle.py](../macOS/tools/vm-source-bundle.py) uses that inventory to collect package sources, fetched patches, pinned nixpkgs definitions, and Tsugumi's image assembly files and build instructions. Package license names come from nixpkgs declarations.

Keep the corresponding source available while distributing the binaries. Before deleting a VM release, check which app releases depend on it. The kernel's source and configuration are described in [kernel/SOURCES.md](../macOS/Container/kernel/SOURCES.md).

## Web bundles

The standalone `web/dist` is built from a checkout rather than published as a separate release. If distributing it separately, collect the license texts of its bundled dependencies using the build's esbuild metafile.

`plotly.min.js` is distributed in upstream's minified form. Its overall license and some dependency notices are included, but not all dependency license texts. Vega's dependency license texts are collected from the lock file.

## Updating dependencies

Update [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) with dependency changes. Review transitive dependencies and font licenses, regenerate the VM inventory and source bundle when changing the boot disk, and inspect the license directory in the distribution before publishing.
