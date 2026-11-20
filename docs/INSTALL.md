# Installation and development builds

## Getting it

**It runs on macOS, Linux and Windows** (Windows through WSL2). The desktop app is for Apple silicon Macs with macOS 26 or later only; on Linux and Windows, use the Web UI of the [`tsugumi` command](#using-it-in-the-browser-the-tsugumi-command).

Preparing the runtime the first time (assembling Tsugumi inside the VM and fetching the Cards' dependencies) needs an internet connection.

### Installing the desktop app (Mac)

Put the latest version from [GitHub Releases](https://github.com/gen740/Tsugumi/releases) in `/Applications`.

Open `Tsugumi.dmg` and drag it to `Applications`, or install with this command:

```bash
curl -fL -o /tmp/Tsugumi.zip https://github.com/gen740/Tsugumi/releases/latest/download/Tsugumi.zip && ditto -x -k /tmp/Tsugumi.zip /Applications && open -g /Applications/Tsugumi.app
```

Releases are signed ad hoc (with the virtualization entitlement needed to run the VM), without a Developer ID signature or Apple's notarisation. An app taken out of a zip downloaded in a browser is stopped by Gatekeeper, so fetch it with `curl` as above, or remove the quarantine attribute after taking it out:

```bash
xattr -dr com.apple.quarantine /Applications/Tsugumi.app
```

From "Browse Demos" on the home screen, choose "Open a Copy" on the demo you want to try, and once the first preparation is done, press "Run" at the top of the screen. On first start, connect the Agent with an Anthropic or OpenAI API key, or choose "Continue with ChatGPT" to use your ChatGPT plan. Plan usage is separate from API key billing; Tsugumi charges nothing. You can also choose "Skip and Start" and connect later under "API" in Settings. In the Web UI, ChatGPT sign-in is available only when the page is opened at localhost or 127.0.0.1 in a browser on the server’s machine; use an API key from a non-local page. The basic operations are in the [README](../README.md#following-an-experiment-on-screen) and [Usage](Usage.md); what data is sent to outside models is in [Security and isolation](security.md#what-is-sent-to-the-model).

### Automatic updates

Use **Check for Updates…** in the application menu or **General → Updates** in Settings to check GitHub and download a newer release. Its checksum, signature, bundle ID and version are verified. Choose **Restart to Update** to install it after orderly runtime shutdown. Checks and installation do not run automatically.

- The folder the app is in (usually `/Applications`) must be writable. An app opened with the quarantine attribute still on (running from elsewhere through App Translocation) cannot update; move it to `/Applications` and remove the quarantine attribute.
- During 0.x, project documents and settings may change without migration ([compatibility policy](../AGENTS.md#compatibility)).

### Using it in the browser (the `tsugumi` command)

A command for using Tsugumi in the [Web UI](WebUI.md) in a browser instead of the app. It needs neither root nor administrator rights.

Mac and Linux:

```bash
curl -fsSL https://github.com/gen740/Tsugumi/releases/latest/download/install.sh | sh
```

Windows (PowerShell):

```powershell
irm https://github.com/gen740/Tsugumi/releases/latest/download/install.ps1 | iex
```

This installs `~/.local/bin/tsugumi`, a command that talks to `tsugumi-hostd` (`tsugumi` alone lists what it does). Open the Web UI in the browser:

```bash
tsugumi web
```

It starts the host when none runs, has it show a page and prints the page's URL, then returns; the host keeps running, like the desktop app's. `tsugumi web <project folder>` shows that project: a page already open on Home takes it, else the browser opens one. `--no-browser` only prints the URL (for a browser on another machine), and `--port` asks for a port. Starts, including starts from the Claude Code plugin, share one lock per host state folder, so concurrent commands use one host.

When a new version comes out, replace it with this command (if it is already the latest, it says so and does nothing). A host that runs keeps its version until it stops.

```bash
tsugumi update
```

- **What is installed**: only the command (`~/.local/bin/tsugumi`) and what it runs (`~/.local/share/tsugumi/<version>.<generation>/`). If `~/.local/bin` is not on `PATH`, it says so; it does not change your shell's settings. `tsugumi update` or installing again replaces it with a new version (it does not update itself). To remove it, see [Uninstalling](#uninstalling).
- **Mac (Apple silicon, macOS 26 or later)**: fetches `tsugumi-macos-arm64.tar.gz`, which includes the command, `tsugumi-hostd`, the built Web UI and the VM's boot disk. Cards run in the same shared Linux VM as the desktop app, so Nix need not be installed separately. The release-built image starts the bundled Tsugumi runtime; first boot initializes its persistent disks. The settings shared with the desktop app, and when the two cannot be used at once, are in [Web UI](WebUI.md#the-tsugumi-command).
- **Linux (x86_64 / aarch64)**: needs [Nix](https://nixos.org/download). It fetches the source, `tsugumi-linux.tar.gz`, and builds it with Nix (the first time takes a few minutes; how it works is in [Web UI](WebUI.md#the-tsugumi-command)).
- **Windows**: needs WSL2 and [Nix](https://nixos.org/download) installed in its Linux. `install.ps1` runs `install.sh` in the default WSL Linux and creates `%USERPROFILE%\.local\bin\tsugumi.cmd` (it does not change `PATH`). `tsugumi web` runs inside WSL just as on Linux, and the page opens in the Windows browser. Keep projects on the WSL file system and pass folders as absolute paths as WSL sees them. It can also be installed from a shell in the WSL Linux with the same `curl … | sh` as on Linux (no `tsugumi.cmd` is made, but the page still opens in the Windows browser).
- Each installation, including the same version again, gets a separate runtime directory. The installer checks the checksum (`.sha256`), builds or unpacks there, and runs `tsugumi --version` before atomically switching `current`. A failed build or version check leaves the current command usable. Old runtimes stay available because a running host can still read their assets; stop hosts and use `tsugumi uninstall --yes` to remove installer-marked generations. Foreign directories under the install root are preserved. To choose a version, use the environment variable `TSUGUMI_VERSION`.

## Building from source

What follows is how to build the app, and how to develop or change Tsugumi itself and its runtime. Unless stated otherwise, run the commands at the root of the repository.

An app built from source is the **development version** (Tsugumi Dev). It is a separate app from the release, with its own support folder (`~/Library/Application Support/Tsugumi-dev/`), so it does not touch the release's VM and both can run at once. The development version does not update itself ("Development and release versions" in [Releasing](../RELEASING.md)).

### Prerequisites

- Nix (with flakes enabled)
- Xcode or the Command Line Tools
- For development image cache downloads: authenticated `gh` (`gh auth login`). If no artifact matches the current definition: a Linux Nix builder, or Apple container (the version is `TSUGUMI_CONTAINER_VERSION` in [vm/guest/versions.env](../vm/guest/versions.env); after `container system start`)

Dependency pins are defined in [flake.lock](../flake.lock), [Package.swift](../ui/macos/Package.swift) and the relevant package lock files.

### Minimal steps

```bash
ui/macos/build-app.sh
```

```bash
open -g dist/Tsugumi.app
```

### Each stage in detail

Enter the development shell and build the core (tsugumid) and the SDK:

```bash
nix develop
```

```bash
cmake -S . -B build -G Ninja && ninja -C build
```

On Linux, the same CMake build also produces the remote listener `build/workerd/tsugumi-workerd`. The packaged listener is available as `nix build .#workerd`; see [Remote execution](RemoteExecution.md) for installation and registration.

The shared VM's release-built guest contains a kernel, read-only rootfs, Tsugumi,
the worker and project tools in one `dist/tsugumi-vm/tsugumi-vm.tar.gz` payload.
The resolver evaluates the full guest definition and verifies local artifacts
before reuse; otherwise it downloads the artifact of that exact identity from
Actions with `gh`. When there is neither, it stops: `--build` builds the same
output here with Nix (Linux) or an Apple container (Mac).
First boot registers the shipped store and starts the prebuilt supervisor,
without a source share or Tsugumi compilation:

```bash
nix run .#guest-image
```

The builder's resources can be set with the environment variables `TSUGUMI_BUILDER_CPUS` (default `8`), `TSUGUMI_BUILDER_MEMORY` (default `16G`) and `TSUGUMI_BUILDER_VOLUME` (the volume for the Nix cache, default `tsugumi-builder-nix`).

`ui/macos/build-app.sh` packages the app into `dist/Tsugumi.app`, including the shared
module host, demos, guest archive/manifest, then
signs it. It resolves the current guest on every build (`nix run .#guest-image`).
Build the guest here first with `nix run .#guest-image -- --build`
(`TSUGUMI_VM_NIX_BUILDERS` uses a configured Linux builder). For release
artifact requirements and signing, see [Releasing](../RELEASING.md).
`--check` and `--kernel` build locally too.

Starting the VM needs a code signature with the virtualization entitlement, so during development too, use the `dist/Tsugumi.app` that `ui/macos/build-app.sh` makes (a binary started directly with `swift run` has no signature and cannot start the VM). `tsugumi-vmctl`, which test scripts use to operate the VM, is built with `vm/macos/tools/build-vmctl.sh`.

The app icon `ui/macos/Resources/AppIcon.icns` is generated from `docs/assets/tsugumi-icon.svg` and committed. After changing the SVG, regenerate it with `swift ui/macos/Resources/tools/make_icon.swift`.

## Checking after building

```bash
tests/ci-local.sh
```

The test suites and commands are in [Repository tests](../tests/README.md).

## Uninstalling

Quit Tsugumi and stop the running `tsugumi-hostd` first. Preview what the command-line installer will remove, then apply it:

```sh
tsugumi uninstall
tsugumi uninstall --yes
```

This removes all runtime generations carrying the installer's marker, their Nix result links on Linux, and `~/.local/bin/tsugumi`. It preserves projects and their run history, settings, API credentials, the Mac shared VM, Nix, and the Nix store. Unrelated files and unmarked directories under the installation root are preserved; each marked generation is removed entirely. A launcher or installation directory that has been replaced or redirected is refused rather than deleted. This command applies to installations made by `install.sh`; manage a Nix profile installation with Nix instead.

On Windows, run `tsugumi uninstall --yes` from Windows through the installed `tsugumi.cmd`. The command removes the installation in the default WSL distribution and deletes the Windows launcher only after success. Running it directly inside WSL leaves the Windows launcher in place; remove `%USERPROFILE%\.local\bin\tsugumi.cmd` separately in that case. WSL and its distributions are preserved.

The native Mac app is separate: move `Tsugumi.app` from Applications to the Trash. Its shared support folder (`~/Library/Application Support/Tsugumi`) is preserved, so reinstalling can reuse the VM and settings. Do not remove that folder if another Tsugumi installation still uses it.
