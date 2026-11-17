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

The release carries the VM image and Tsugumi's own source through GitHub's tag archive; unmodified OSS refers to upstream sources, without source archives, mirrors, storage or repacking by Tsugumi.
`vm-licenses.tar.gz`, also expanded into the app and CLI's `licenses/vm-guest/`, lists runtime package names, versions and nixpkgs SPDX license identifiers (or missing metadata), with one text per identifier, Tsugumi's fixed MIT notice including hosq, and the kernel configuration.
Required license and copyright notices are retained.

## Web bundles

The `tsugumi` CLI packages the Web UI. `cli/package-macos.sh` collects its npm license texts with `ui/web/scripts/licenses.mjs --all`; preserve those notices when distributing `ui/web/dist` separately too.

`plotly.min.js` is distributed in upstream's minified form. Its overall license and some dependency notices are included, but not all dependency license texts. Vega's dependency license texts are collected from the lock file.

## Updating dependencies

Update [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) with dependency changes. Review transitive dependencies and font licenses, regenerate the VM inventory when changing the boot disk, and inspect the license directory in the distribution before publishing.
