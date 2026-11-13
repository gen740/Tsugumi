# Installation and development builds

## Getting it

**It runs on macOS, Linux and Windows** (Windows through WSL2). The desktop app is for Apple silicon Macs with macOS 26 or later only; on Linux and Windows, use the Web UI of the [`tsugumi` command](#using-it-in-the-browser-the-tsugumi-command).

Preparing the runtime the first time (assembling Tsugumi inside the VM and fetching the Cards' dependencies) needs an internet connection.

### Installing the desktop app (Mac)

Put the latest version from [GitHub Releases](https://github.com/gen740/Tsugumi/releases) in `/Applications`.

Open `Tsugumi.dmg` and drag it to `Applications`, or install with this command:

```bash
curl -fL -o /tmp/Tsugumi.zip https://github.com/gen740/Tsugumi/releases/latest/download/Tsugumi.zip && ditto -x -k /tmp/Tsugumi.zip /Applications && open /Applications/Tsugumi.app
```

Releases are signed ad hoc (with the virtualization entitlement needed to run the VM), without a Developer ID signature or Apple's notarisation. An app taken out of a zip downloaded in a browser is stopped by Gatekeeper, so fetch it with `curl` as above, or remove the quarantine attribute after taking it out:

```bash
xattr -dr com.apple.quarantine /Applications/Tsugumi.app
```

From "Browse Demos" on the home screen, choose "Open a Copy" on the demo you want to try, and once the first preparation is done, press "Run" at the top of the screen. To use the Agent, set the model provider and API key under "API" in the settings, or choose the ChatGPT plan and sign in with "Continue with ChatGPT". The basic operations are in the [README](../README.md#following-an-experiment-on-screen) and [Usage](Usage.md); what data is sent to outside models is in [Security and isolation](security.md#what-is-sent-to-the-model).

### Automatic updates

Releases check GitHub's latest release after starting and every six hours. If there is a new version, it is downloaded, its checksum, signature, bundle id and version number are checked, and it replaces the app **the next time you quit** (the VM is stopped at that point, so a running computation is never interrupted by an update). To update at once, choose "Restart to Update" on the home screen or "Restart to update to …" in the application menu. Under "General" → "Updates" in the settings you can see the current version, check by hand and turn automatic checks on or off.

- The folder the app is in (usually `/Applications`) must be writable. An app opened with the quarantine attribute still on (running from elsewhere through App Translocation) cannot update; move it to `/Applications` and remove the quarantine attribute.
- The ad hoc signature changes with each version, so the first time an updated version uses an API key, macOS asks once for access to the keychain (choose "Always Allow" and that version will not ask again).
- The stable release starts at v1.0.0. During the current 0.x versions, compatibility of project documents and settings is not kept, and nothing is migrated ([AGENTS.md](../AGENTS.md)).

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

This installs `~/.local/bin/tsugumi`. Run it to start the server and open the browser:

```bash
tsugumi
```

Stop it with Ctrl-C. If it is already running, it only opens a new page. `tsugumi <project folder>` opens that project.

When a new version comes out, replace it with this command (if it is already the latest, it says so and does nothing). A running `tsugumi` opens with the new version after you stop and start it again.

```bash
tsugumi update
```

- **What is installed**: only the command (`~/.local/bin/tsugumi`) and what it runs (`~/.local/share/tsugumi/<version>/`). If `~/.local/bin` is not on `PATH`, it says so; it does not change your shell's settings. `tsugumi update` or installing again replaces it with a new version (it does not update itself). To remove it, see [Uninstalling](#uninstalling).
- **Mac (Apple silicon, macOS 26 or later)**: fetches `tsugumi-macos-arm64.tar.gz`, which includes Node.js and the VM's boot disk. Cards run in the same shared Linux VM as the desktop app, so Nix need not be installed separately. The first start takes a few minutes while Tsugumi is assembled inside the VM. The settings shared with the desktop app, and when the two cannot be used at once, are in [Web UI](WebUI.md#the-tsugumi-command).
- **Linux (x86_64 / aarch64)**: needs [Nix](https://nixos.org/download). It fetches the source, `tsugumi-linux.tar.gz`, and builds it with Nix (the first time takes a few minutes; how it works is in [Web UI](WebUI.md#the-tsugumi-command)).
- **Windows**: needs WSL2 and [Nix](https://nixos.org/download) installed in its Linux. `install.ps1` runs `install.sh` in the default WSL Linux and creates `%USERPROFILE%\.local\bin\tsugumi.cmd` (it does not change `PATH`). `tsugumi` runs inside WSL just as on Linux, and the page opens in the Windows browser. Keep projects on the WSL file system and pass folders as absolute paths as WSL sees them. It can also be installed from a shell in the WSL Linux with the same `curl … | sh` as on Linux (no `tsugumi.cmd` is made, but the page still opens in the Windows browser).
- Downloaded files are installed after their checksum (`.sha256`) is checked. To choose a version, use the environment variable `TSUGUMI_VERSION`.

## Building from source

What follows is how to build the app, and how to develop or change Tsugumi itself and its runtime. Unless stated otherwise, run the commands at the root of the repository.

An app built from source is the **development version** (Tsugumi Dev). It is a separate app from the release, with its own support folder (`~/Library/Application Support/Tsugumi-dev/`), so it does not touch the release's VM and both can run at once. The development version does not update itself ("Development and release versions" in [Releasing](../RELEASING.md)).

### Prerequisites

- Nix (with flakes enabled)
- Xcode or the Command Line Tools
- Only to build the VM's boot disk (when there is no [published one](https://github.com/gen740/tsugumi-vm)): Apple container (the version is `TSUGUMI_CONTAINER_VERSION` in [macOS/Container/versions.env](../macOS/Container/versions.env); after `container system start`)

Dependency pins are defined in [flake.lock](../flake.lock), [Package.swift](../macOS/Package.swift) and the relevant package lock files.

### Minimal steps

```bash
macOS/build-app.sh
```

```bash
open dist/Tsugumi.app
```

### Each stage in detail

Enter the development shell and build the core (tsugumid) and the SDK:

```bash
nix develop
```

```bash
cmake -S . -B build -G Ninja && ninja -C build
```

The shared VM's boot disk (a kernel and a small rootfs with Nix, written to `dist/tsugumi-vm/`). If there is a published one at [gen740/tsugumi-vm](https://github.com/gen740/tsugumi-vm), it is downloaded; otherwise it is built with Nix inside a container (no separate Linux host or remote builder is needed). It changes only when the pinned nixpkgs or `macOS/Container/vm-boot.nix` changes; after changing them, publish it with `macOS/tools/publish-vm.sh`. Tsugumi itself (tsugumid and the rest) is built by Nix inside the VM on its first boot, from the shared source (`macOS/Container/vm-system.nix`; rebuilt only when the source changes):

```bash
macOS/Container/build-image.sh --vm
```

The builder's resources can be set with the environment variables `TSUGUMI_BUILDER_CPUS` (default `8`), `TSUGUMI_BUILDER_MEMORY` (default `16G`) and `TSUGUMI_BUILDER_VOLUME` (the volume for the Nix cache, default `tsugumi-builder-nix`).

`macOS/build-app.sh` packages the app into `dist/Tsugumi.app`. It bundles the shared module host, the demos, the VM's boot disk and the source the VM assembles Tsugumi from (`macOS/Container/vm-source.txt`), and signs it ad hoc for this machine. If there is no boot disk, run `build-image.sh --vm` first. With `--release` it builds a release (used by the release workflow; [Releasing](../RELEASING.md)).

Starting the VM needs a code signature with the virtualization entitlement, so during development too, use the `dist/Tsugumi.app` that `macOS/build-app.sh` makes (a binary started directly with `swift run` has no signature and cannot start the VM). `tsugumi-vmctl`, which test scripts use to operate the VM, is built with `macOS/tools/build-vmctl.sh`.

The app icon `macOS/Resources/AppIcon.icns` is generated from `docs/assets/tsugumi-icon.svg` and committed. After changing the SVG, regenerate it with `swift macOS/Resources/tools/make_icon.swift`.

## Checking after building

```bash
tests/ci-local.sh
```

The test suites and commands are in [Repository tests](../tests/README.md).

## Uninstalling

Quit Tsugumi and stop any running `tsugumi` server first. Preview what the command-line installer will remove, then apply it:

```sh
tsugumi uninstall
tsugumi uninstall --yes
```

This removes the installed runtime, its Nix result link on Linux, and `~/.local/bin/tsugumi`. It preserves projects and their run history, settings, API credentials, the Mac shared VM, Nix, and the Nix store. Unrelated files alongside the installed version directory are preserved; the installed version directory itself is removed entirely. A launcher or installation directory that has been replaced or redirected is refused rather than deleted. This command applies to installations made by `install.sh`; manage a Nix profile installation with Nix instead.

On Windows, run `tsugumi uninstall --yes` from Windows through the installed `tsugumi.cmd`. The command removes the installation in the default WSL distribution and deletes the Windows launcher only after success. Rerun the Windows installer to refresh the launcher when updating from a version without this command; `tsugumi update` only updates the WSL installation. Running it directly inside WSL leaves the Windows launcher in place; remove `%USERPROFILE%\.local\bin\tsugumi.cmd` separately in that case. WSL and its distributions are preserved.

The native Mac app is separate: move `Tsugumi.app` from Applications to the Trash. Its shared support folder (`~/Library/Application Support/Tsugumi`) is preserved, so reinstalling can reuse the VM and settings. Do not remove that folder if another Tsugumi installation still uses it.
