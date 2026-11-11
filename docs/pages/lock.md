# Pinned versions

The versions of the runtime and its dependencies that are pinned to keep things reproducible. How to install and build the app is in [INSTALL.md](install.md).

| Item | Pinned version / revision |
| --- | --- |
| Platforms | macOS (Apple silicon, macOS 26 or later; checked on macOS 27.2, M3 Max), Linux (x86_64 and aarch64, needs Nix; checked in an OrbStack VM (Ubuntu 26.04, aarch64), on a friend's Ubuntu (x86_64) and on NixOS (x86_64, over SSH)), Windows (needs WSL2 and Nix; verified in WSL2, with Nix without its daemon) |
| Node.js (bundled with the Mac's `tsugumi` command) | `cli/node-version.env` (the official build, checked by checksum). On Linux and Windows, the pinned nixpkgs' `nodejs` |
| Shared VM | Linux kernel 6.12.115 (`macOS/Container/kernel.nix`; SELinux and io_uring disabled, unprivileged BPF off) + a read-only rootfs (erofs, `macOS/Container/vm-boot.nix`; a boot disk with only Nix from nixpkgs, published at gen740/tsugumi-vm). Tsugumi itself is built inside the VM from the nixpkgs of the same flake.lock (`macOS/Container/vm-system.nix`) |
| Builder for the VM image | `docker.io/nixos/nix:2.31.2` running on Apple container 0.6.0 (only for building the boot disk during development; not used when the app runs) |
| nixpkgs | `1c89083e451e891152365b799595cf7790a43325` (`flake.lock`; the Cards' default `tsugumi:default` is the same revision) |
| FlatBuffers | 25.12.19 (C++ / Python from nixpkgs; Swift pinned to the same version with SwiftPM) |
| hosq (C++ JSON) | `CLI20-dev/hosq@1eb8942` (a flake input) |
| Dawn (WebGPU) | `v20261023.194543` (`ui/renderer/CMakeLists.txt`, Metal backend) |
| SwiftTerm (terminal display, MIT) | 1.11.2 (pinned because later versions need Xcode's Metal Toolchain to be fetched separately to compile shaders) |
| Swift Markdown / SwiftMath (Markdown and maths in the chat, sticky notes and Cards) | 0.7.3 / 1.7.3 (`macOS/Package.swift`) |
| SwiftTreeSitter and the language grammars (syntax highlighting in the editor) | SwiftTreeSitter 0.25.0; each grammar at its own version (`macOS/Package.swift`) |
| xterm.js / addon-fit / node-pty (the Web UI's terminal, MIT) | 5.5.0 / 0.10.0 / 1.1.0 (`web/package-lock.json`) |
| libgit2 (versions) | 1.9.7 (the pinned nixpkgs' `libgit2`) |
| plotly.js / Vega / Vega-Lite / Vega-Embed (display of Card charts) | 3.6.0 / 6.4.0 / 6.4.3 / 7.3.0 (`web/package-lock.json`; the Mac app copies the same files in `build-app.sh`) |
