# Development

A guide to working on this repository: code locations, releases, documentation and tests.
Installing and building the app are in [INSTALL.md](INSTALL.md), the pinned versions of dependencies in [Lock.md](Lock.md), and the architecture and mechanisms in [Features](features.md#1-architecture).
File paths and commands on this page are relative to the root of the repository.

## Directories

```text
CMakeLists.txt, flake.nix/flake.lock  C++ build (CMake/Ninja) and pinned dependencies (Nix)
protocol/          tsugumi.fbs (project RPC) / agent.fbs (host-side core and Agent) / worker.fbs / data.fbs / tsugumi_enums.fbs
core/              tsugumid (project core) and tsugumi-agentd (host-side core);
                   src/host/ (project lifecycle), src/agent/ (conversations and tools)
client/            Shared C++ RPC client, C interface and WebAssembly bindings
sdk/               Card ABI, common runtime (libtsugumi_card), C++ codegen (card.hh), Python, R, Julia, Ruby, Go, Rust and C# SDKs and embedded shims, Nix SDK, tsugumi-worker
macOS/             SwiftPM: Sources/App (SwiftUI), Sources/RPC, Sources/Launcher,
                   Sources/VMHost, Sources/VMCtl, Container/ (image definitions, builds, versions), CoreTests/, Tests/
ui/                modules/ (shared isolated web-module host: browser iframe and WKWebView), templates/ (shared Card scaffolds)
agent/             Shared Agent prompts, contracts and reference material
macOS/Sources/AgentClient/  Native adapter to the host-side core RPC
web/               Browser UI and host server (Agent RPC and platform adapters; shared material in agent/Shared)
remote/            Python execution daemon and durable job journal for remote Linux hosts
cli/               The tsugumi command: installers (install.sh, install.ps1), the Mac launch script, scripts that make the distributions
plugins/tsugumi/   The Claude Code plugin (installed from .claude-plugin/marketplace.json)
nix/               core.nix: package definitions of tsugumid, tsugumi-agentd, tsugumi-worker and the Card SDK
                   flake/: the flake's outputs as flake-parts modules (packages, checks, dev shell, docs apps)
tests/             core/, python/, static/, remote/, prompt/, fixtures/, tools/: contracts, integration, packaging, static checks, remote jobs, prompt probes and fixtures
demos/ja, demos/en The demos, a set per UI language (the same demos and files, their text in one language): the UI's demo list (index.json);
                   basic, science and Plots (matplotlib, Plotly, Altair) demos, and Draft demos
docs/              Documents (listed in docs/README.md). docs/assets/ has the icon, key visuals and screenshots;
                   docs/site/ has the Astro site and the moving figures (output: build/docs/, not tracked)
```

Generated bindings (C++/Python/Swift) and BFBS are made by `flatc` into each build's output and are not in Git.

## Releases

- **The version number** is only in `VERSION` (read by the core, Nix, CMake and the app).
- **Making a release**: in a checkout of origin/main, `macOS/tools/release.sh 0.2.0` rewrites `VERSION`, commits it and pushes it to main with the tag `v0.2.0`. Without a checkout, `gh workflow run release.yml -f version=0.2.0` (Actions does the same). Either way the release workflow (`.github/workflows/release.yml`) builds the app and creates a GitHub Release, which installed releases find and update to automatically (`macOS/Sources/App/Model/Updater.swift`).
- **Making a release candidate**. Run `macOS/tools/release.sh 0.2.0-rc.1` from a checkout of origin/main. `VERSION` and the packaged app and command report `0.2.0`. The workflow publishes the suffixed tag as a GitHub prerelease. Download the app from the candidate's release page. Set `TSUGUMI_VERSION` to `0.2.0-rc.1` when running a CLI installer for the candidate. The app's automatic updater follows stable releases. The app needs a manual reinstall to switch to the stable release of the same numeric version. The `tsugumi update` command uses the latest stable release.
- **The `tsugumi` command**: the same workflow puts `cli/install.sh`, `cli/install.ps1`, and the `tsugumi-macos-arm64.tar.gz` and `tsugumi-linux.tar.gz` made by `cli/package-macos.sh` and `cli/package-linux.sh` (each with a `.sha256`) on the release. The scripts are attachments of the release rather than files on the documentation site: the site updates on every push to main, so it would show scripts for versions with no distributions yet, whereas as attachments the scripts and the distributions are always the same version and arrive as they are through `releases/latest/download/`. The Node.js version and checksums are in `cli/node-version.env`. To try it locally, pass a folder holding the distributions as `TSUGUMI_INSTALL_BASE=file://…`, and keep the VM apart with `TSUGUMI_SUPPORT_DIR`.
- **The VM's boot disk** (`vm-boot.nix` and the pinned nixpkgs): after changing it, publish it to [gen740/tsugumi-vm](https://github.com/gen740/tsugumi-vm) before releasing, with `macOS/Container/build-image.sh --vm` and `macOS/tools/publish-vm.sh` (which also publishes the list of licenses and the corresponding source with the disk). The runners cannot use Apple container, so the release build fails without a published disk.
- **Development and release versions** (`AppChannel.swift`): `build-app.sh` makes the development version by default (`Tsugumi Dev`, bundle id `app.tsugumi.Tsugumi.dev`, `TsugumiChannel` `dev` in `Info.plist`), and with `--release` the release (`Tsugumi`, `app.tsugumi.Tsugumi`, `release`). The support folder (VM, store, control folders, conversation history) is `~/Library/Application Support/Tsugumi-dev/` for the development version and `Tsugumi/` for the release, and user settings (UserDefaults) are separate per bundle id too. Checks and tests during development (UI scripts, smoke, `swift test`) use the development version, so they never touch the VM of the release you use. The Python tests' shared VM is `build/test-vm`.
- **Signing development builds**: by default both are signed ad hoc. With ad hoc signing, the designated requirement is the hash of that build's binary, so the login keychain treats every rebuild as a different app and shows a permission dialog (reading the API key, for example). The development version can be signed with a certificate in this Mac's keychain (Apple Development or the like) named in the environment variable `TSUGUMI_CODESIGN_IDENTITY`: `TSUGUMI_CODESIGN_IDENTITY="Apple Development" macOS/build-app.sh` (part of the certificate's name, or its SHA-1, as `codesign --sign` takes; list usable certificates with `security find-identity -v -p codesigning`). The designated requirement becomes the certificate and bundle id, unchanged across rebuilds. `--release` ignores the variable and stays ad hoc (release artifacts do not depend on the environment). The first time after switching from ad hoc, the keychain dialog appears once (choose "Always Allow", or save the key again); after that it does not. The `com.apple.security.virtualization` entitlement the VM needs is attached with either signature.
- **Automatic updates** happen only in releases. 20 seconds after launch and every six hours, the app reads `api.github.com/repos/gen740/Tsugumi/releases/latest`, and if a version newer than the current one (not a draft or pre-release) has `Tsugumi.zip` and `Tsugumi.zip.sha256`, downloads them into a temporary folder on the same volume as the app and checks the checksum, `codesign --verify --deep --strict`, the bundle id, `TsugumiChannel` and the version number. The replacement happens in `applicationWillTerminate` (after stopping the VM) with `FileManager.replaceItemAt`; for "Restart to Update", the new app is opened after waiting for this process to end. No checks happen while a UI script or smoke run is going.

## The documentation site

The Markdown files in `docs/` are the source documents. The repository's `README.md` is the entry point; `THIRD_PARTY_NOTICES.md` stays with the license. Edit these sources, never a generated copy. [docs/README.md](README.md) supplies the navigation groups and descriptions.

The site is an Astro project in `docs/site/`, with its own pinned dependencies. Nix supplies Node.js and builds the site's packages from `docs/site/package-lock.json` ([nix/docs-site.nix](../nix/docs-site.nix)); `docs/site/node_modules` is a link to them, and nothing is installed with npm.

```bash
nix run .#dev-doc
```

Astro serves the site at `http://localhost:4321` and reloads when documents, components or illustrations change. To build and check the static output:

```bash
nix run .#build-doc
nix run .#test-doc
```

- **Output:** `build/docs/` contains the HTML, linked Markdown downloads, `llms.txt` and static assets. It is ignored by Git and can be deleted and rebuilt. After one of the commands above has made the link, `npm --prefix docs/site run preview` in the dev shell serves that output locally.
- **Content:** `src/content.config.ts` loads the source documents into an Astro content collection. Astro renders Markdown; the `.astro` components provide the common layout, navigation and home page. `src/lib/documents.mjs` defines the story illustrations and resolves repository links for the website and Markdown downloads.
- **Publication:** [.github/workflows/docs.yml](../.github/workflows/docs.yml) builds and checks the site on pull requests and publishes `build/docs/` on pushes to `main`. Set the repository's Pages source to **GitHub Actions**. No generated pages are committed.
- **App help:** `macOS/build-app.sh` builds the same site and `macOS/tools/bundle-docs.sh` bundles its story pages and assets. The development app reads `build/docs/` when no bundled help is present. Relative URLs let the pages work under both GitHub Pages and the app's local URL scheme.
- **Illustrations:** the Canvas modules in `docs/site/js/` run only on pages that use them. Story figure positions are defined in `STORY` in `src/lib/documents.mjs`; a missing anchor phrase fails the build.
- **Demo player:** `docs/site/player/` builds the interactive gallery from the Web UI. Its shared outputs (`docs/site/js/player.js`, `docs/site/player.css`, `docs/site/player-data/`) are also used by the Web UI and native thumbnails. Regenerate them with `node docs/site/player/build.mjs` after changing the player inputs; `node docs/site/player/build.mjs --check` verifies them. This needs `web/node_modules` and `web/src/gen`.
- **Without JavaScript:** document text and links remain readable. Animation respects reduced motion.

## Tests

Run `tests/check-fast.sh` before every push: the static tests (`tests/static`: tests that only read files of the repository, which CI runs first in its own job) and the Nix checks of the Web UI and the site player (`--no-nix` skips these). Run the platform checks with `tests/ci-local.sh`. On macOS this runs the Mac checks locally and the Linux checks in Docker or OrbStack. Use `tests/ci-local.sh mac` or `tests/ci-local.sh linux` for one platform, and `tests/ci-local.sh agent` for the public Agent RPC fixtures.

The CI definitions are [ci.yml](../.github/workflows/ci.yml) and [ci-platform.yml](../.github/workflows/ci-platform.yml).

| Location | Coverage |
| --- | --- |
| [tests/core/](../tests/core/README.md) | Core behavior, public RPC and integration tests |
| `tests/python/` | Packaging, release, licenses and host integration |
| `tests/static/` | Repository contracts and generated-file consistency |
| `tests/remote/` | Remote execution daemon, job journal and scheduling |
| `macOS/CoreTests/`, `macOS/Tests/` | Agent, RPC and app behavior |
| `web/` | Browser UI type checks and behavior contracts |
| `tests/prompt/` | Offline prompt probe scoring |

For documentation changes, run `nix run .#build-doc` and `nix run .#test-doc`. Changes to the demo player also require `node docs/site/player/build.mjs --check`.

### Manual tools

- `python3 tests/python/measure_language_workers.py`: measure the Collatz R/Julia workers with `TSUGUMI_SDK_DIR` and `TSUGUMI_WORKER` selecting the SDK and worker.
- `node web/tools/module-perf.mjs <output-directory> [baseline-ref]` and `swift macOS/tools/perf/module-viewer.swift <output-directory>/modules`: build display performance fixtures and exercise the viewer in offscreen WKWebViews.
- `nix develop -c /usr/bin/swift tests/tools/wkwebgpu.swift`: probe WebGPU and adapter availability in an offscreen WKWebView.
- `python3 sdk/codegen/scaffold_go.py`: synchronize the Swift and TypeScript Go scaffolds after editing `sdk/go/template/` (`--check` verifies them).
- `python3 sdk/codegen/scaffold_rust.py`: synchronize the Swift and TypeScript Rust scaffolds after editing `sdk/rust/template/` (`--check` verifies them).
