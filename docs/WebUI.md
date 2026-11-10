# Web UI

`web/` is a client that provides, in the browser, the same screens, operations and shortcuts as the macOS app.
File paths and commands on this page are relative to the root of the repository.

## Architecture

The `web/` server runs a `tsugumid` per project in one of two ways. On a Mac, the [`tsugumi` command](#the-tsugumi-command) runs it inside the same shared Linux VM as the macOS app; otherwise (the `tsugumi` command on Linux, or starting from the repository) it runs **directly on this host**. The diagram below and "Isolation (macOS host)" describe the latter.
The browser connects **directly** to `tsugumid`'s WebSocket `/rpc` with the FlatBuffers RPC, as an equal client using the same contract as the macOS app and the Agent.

```text
Browser (React UI + WebGPU rendering) ── WebSocket /rpc (FlatBuffers) ── tsugumid (runs directly on the host, one per project)
        └─ HTTP /api ── Node server (startup, files, Agent) ── Unix socket ──┘
```

- `tsugumid` allows only the UI's origins (`http://127.0.0.1:<port>` and `http://localhost:<port>`) with `--allow-origin`, and the token is sent only in SessionHello (never in a URL or a log).
- The Node server (`web/server/`) takes on only what a browser cannot do: operations on project folders (creating, copying demos, choosing folders, reading a project to import as a Card in a Card), starting and stopping `tsugumid`, import grants, loading Skills, the Agent and the model's API key, and signing in to ChatGPT (`chatgpt.json` in the data folder, 0600). It listens only on `127.0.0.1`, checks the `Host` header, and requires, on APIs that change something, a header only a same-origin page can send.
- The Agent runs on the server side, and the tool definitions and the JSON ⇄ FlatBuffers conversion are made from `tsugumi.bfbs` as in the macOS version. It connects to `tsugumid` over a Unix socket.
- Line charts port the WGSL and vertex generation of the macOS version's Dawn rendering layer (`ui/renderer`) to WebGPU (Chromium's WebGPU implementation is Dawn). In a browser without WebGPU, the reason is shown.

## The tsugumi command

The command installed by `curl … | sh` ([Installation](../INSTALL.md#using-it-in-the-browser-the-tsugumi-command)). Run without arguments, it starts the server and opens the browser. If a server is already running with the same data folder, it has that server open a page and exits. If the usual port (8780) is taken, it uses a free one. `tsugumi update` runs the latest release's `install.sh` to replace it with the new version (if it is already the latest, it says so and fetches nothing). A running server stays on the old version; stop it and start it again to open with the new one.

The command is the single `tsugumi` on Mac, Linux and Windows alike. How it runs is decided by the environment, never by the user, so there are no other names.

**On a Mac**, the server (`web/server/vm.ts`) runs `tsugumi-vmhost` (`macOS/Sources/VMHost`) over standard input and output. Running the VM needs a signature with the virtualization entitlement, so only the VM is in a separate process. Inside, it is the same `EnvironmentManager` as the app.

```text
Browser ── WebSocket /rpc ── loopback bridge (tsugumi-vmhost) ── vsock ── tsugumid (inside the shared VM, one per project)
   └─ HTTP /api ── Node server ── stdin/stdout ── tsugumi-vmhost
```

- Card isolation, sharing Mac folders and the terminal are the same as in the macOS app ([Security](security.md)). Only the page's origin is passed to the VM's `tsugumid` with `--allow-origin`. The app passes no origin when it starts one, so a browser cannot connect to it.
- The Agent runs on the server and connects to `tsugumid` with a WebSocket and the token.

What it shares with the desktop app (the release) and what it does not:

| | Where | With the desktop app |
| --- | --- | --- |
| API keys | Keychain (service `app.tsugumi.agent`) | Shared. It reads the item the app saved with `security`, so macOS may ask the first time |
| VM, Nix store, control folders | `~/Library/Application Support/Tsugumi/` (change with `TSUGUMI_SUPPORT_DIR`) | Shared |
| Recent projects | `recent-projects.json` in the same folder | Shared |
| Where projects are kept | `~/Documents/Tsugumi` | Shared |
| Language, provider, model, effort, Fast, web search, auto-approval of permissions, version summaries | The app's UserDefaults (`app.tsugumi.Tsugumi`) | Shared (`tsugumi-vmhost` reads and writes them) |
| Conversation history, Skill folders, ChatGPT sign-in, the guide's state | `~/Library/Application Support/Tsugumi/web/` | Not shared |

As `vm.lock` says, only one app at a time can use the VM. If the desktop app has a project open, the page says so, and the other way round. Stopping the server stops the VM too. If the desktop app and the `tsugumi` command are different versions, switching between them has the VM assemble that version of Tsugumi again (Nix's cache makes it short from the second time).

**On Linux**, `tsugumid` built with Nix runs directly on this host, and Cards are isolated with bubblewrap (`nix/cli.nix`). The terminal's `node-pty` is built with it. Data is in `~/.local/share/tsugumi-web`, and the API key is a 0600 file in it. Installation, connecting from a browser and running demos were checked in an OrbStack VM (Ubuntu 26.04, aarch64). It was also run on x86_64 Ubuntu (a friend's environment).

**On Windows**, `tsugumi.cmd` runs the Linux version inside WSL2 (`cli/install.ps1`). `tsugumid` needs Nix and bubblewrap, so it does not run on native Windows. The server recognises WSL by the environment variable `WSL_DISTRO_NAME` and opens the Windows browser; the page and `tsugumid`'s ports are reached through WSL's localhost forwarding. It has been checked on a friend's Windows as far as opening a demo ([#159](https://github.com/gen740/Tsugumi/issues/159)).

### Running it on another machine, over SSH

You can run `tsugumi` on a Linux machine and use it from the browser at hand through SSH port forwarding. On an x86_64 NixOS machine, this setup was checked through running whole demos, intentional failures, partial runs, restoring results after a reload, and the terminal.

Besides the server (port 8780), the page connects directly by WebSocket to the `tsugumid` of the open project. `tsugumid` listens on a free port of that machine's `127.0.0.1`, and the number changes each time a project is opened. Forwarding only 8780 is therefore not enough to connect to a project: also forward the project's port shown on screen.

```bash
ssh -L 8780:127.0.0.1:8780 -L <port shown>:127.0.0.1:<port shown> <machine>
```

## Building and starting

How to run it from the repository (using this host's `tsugumid`).

Prerequisites: Nix (with flakes enabled), Node.js 22 or later, and a browser with WebGPU (Chrome / Edge / Safari 26 or later).

```bash
nix develop --command bash -c 'cmake -S . -B build -G Ninja && ninja -C build tsugumid tsugumi-worker'
```

```bash
cd web && npm install && nix develop --command npm run gen && npm run build
```

The terminal's `node-pty` is a native module. On macOS, a prebuilt one is in the distribution; on Linux it is built during `npm install` (needs a C++ compiler and Python). If it cannot be built, `npm install` and the Web UI still work, and opening the terminal shows the reason.

```bash
node web/dist/server.js --open
```

Open the `http://127.0.0.1:8780/?key=…` shown at startup (`--open` opens it automatically). `--port` sets the port, and a project folder as an argument opens that project at once. If the core's binaries are missing, they can also be built from the setup screen.

`key` is a key made at each start. It is replaced by a cookie only this page sends (HttpOnly, SameSite=Strict) and disappears from the address bar. All of `/api/` (GET and event subscriptions included) requires this cookie, so other users of the same machine and pages of other sites cannot use it. Calls that change something require a dedicated header as well. Responses carry a CSP (`frame-ancestors 'none'`) and `nosniff`.

The [Claude Code plugin](claude-code.md) finds a running server through the `server.json` it puts in the data folder (how far that key reaches: [Security §8](security.md#the-agents-permission-boundary)).

| Environment variable | Meaning |
| --- | --- |
| `TSUGUMID` / `TSUGUMI_WORKER` / `TSUGUMI_SDK_DIR` | Where the binaries and the SDK are (by default `build/` and `sdk/`) |
| `TSUGUMI_VM_X86=qemu` | The Mac's shared VM runs x86_64-linux Cards with qemu instead of Rosetta (by default Rosetta when it is installed; [Another system](features.md#another-system-x86_64-linux)). Also read by the desktop app |
| `TSUGUMI_VMHOST` / `TSUGUMI_VM_IMAGE` / `TSUGUMI_VM_SOURCE` / `TSUGUMI_SUPPORT_DIR` | `tsugumi-vmhost`, the boot disk, the source and the support folder when using the Mac's shared VM (set by `cli/tsugumi-macos`) |
| `TSUGUMI_WEB_DATA` | Where settings, recent projects, conversations and control folders are kept (default on macOS: `~/Library/Application Support/Tsugumi/web`) |
| `ANTHROPIC_API_KEY` | If set, used as the API key (otherwise, on macOS, the same Keychain item as the app; elsewhere, a 0600 file in the data folder) |
| `TSUGUMI_CHATGPT_ISSUER` / `TSUGUMI_CHATGPT_API` | Where ChatGPT sign-in and plan requests go (for tests; by default `https://auth.openai.com` and `https://api.openai.com`) |

The TypeScript bindings and `tsugumi.bfbs` are generated into `web/src/gen` by `npm run gen` (`flatc` 25.12.19) and are not in Git.

## Isolation (macOS host)

Isolation when running directly on a Mac host from the repository. The `tsugumi` command uses the VM, so this does not apply to it.

A `tsugumid` running directly on the host isolates `sandbox: true` Cards with `sandbox-exec` (Seatbelt; deprecated, but it works on current macOS) instead of bubblewrap (`core/src/worker_proc.cc`).

- No network (unless the Card allows it).
- Readable: only `/nix/store`, the run's and inputs' areas, system libraries and the worker itself. Not the home folder or the project's documents.
- Writable: only the worker's own folder (not its scripts) and the run's output folder (`runs/<run>/work/…/out`). On Linux the output folder is passed through `/proc/self/fd`; macOS has none, so it is passed by path.
- Unlike bubblewrap, checking whether a file exists (`stat`) is allowed everywhere (contents cannot be read), and uids are not separated.
- Sharing a Mac folder with a Card (`/host/<name>`) needs the shared VM, so it is not available in a Web UI running directly on a Mac host (it is with the `tsugumi` command). Folders cannot be added in a Card's settings, and a Card that already has one says so (running it fails). On Linux with bubblewrap it should work without the shared VM (not checked here).
- A Card allowed the network still cannot connect to this machine itself (so it cannot touch tsugumid or the web server): not loopback, nor its LAN or VPN addresses, nor `0.0.0.0` (Seatbelt's `localhost` means all of this machine's addresses). Unix sockets are closed too. The one exception is `mDNSResponder`'s socket (`/var/run/mDNSResponder`): name resolution on macOS goes through it, so closing it would make names unresolvable. That socket is also the gateway to DNS-SD, so through the same path a Card can browse and advertise services on the LAN.
- The only Mach service a Card allowed the network can use is `com.apple.trustd.agent`, used for certificate validation. It cannot reach the clipboard, LaunchServices (opening apps and URLs) or Apple Events. Name resolution and HTTPS work with this (checked with Python, curl, Go and NSURLSession). On the other hand, as with Cards without network, user information (`getpwuid`) cannot be looked up, so `ssh` fails with `No user exists for uid`. System proxy settings are not read.
- Three paths cannot be closed. On networks where they matter, do not give someone else's Card `network: true`.
  - **Other devices on the same LAN or VPN can be reached.** Seatbelt's destinations can only be `*` and `localhost`; specific addresses or ranges cannot be denied.
  - **UDP broadcast and multicast reach this machine too.** A service listening for UDP on `0.0.0.0` can be reached and its replies received. There is no way to stop only broadcasts, and stopping them would mean closing UDP altogether, so it is left open. tsugumid and the web server do not use UDP.
  - **A Card can open a listening socket.** Connecting to it from this machine was confirmed; other devices on the LAN can reach it too if the firewall lets them.
- For folders attached to the chat, tsugumid reads only what the server wrote into `attached.json` in the control folder (it cannot be changed through RPC).
- A `sandbox: false` Card runs on this machine with the user's own permissions. Before turning off "Restrict file access" in a Card's settings, confirm this (`tsugumid` does not judge it). To run someone else's Cards on Linux, run `tsugumid` inside an isolated container.

## Several projects

Each browser tab can open a different project (how this corresponds to the app's windows: [Several projects in Features](features.md#several-projects)). ⌘O, demos and duplicates from a project's tab open in a new tab, and "Open Home in a New Tab" on the Canvas adds a Home tab. A browser cannot bring another tab to the front, though, so the same project can be open in two tabs (equal clients of the same `tsugumid`; a save in one shows in the other). Going back Home closes that project's windows, such as Card settings. If no other tab shows the same project, the chat's work and the terminal are stopped, and so is `tsugumid` (tabs of child Workflows close with it).

The server runs one `tsugumid` per project and knows which tab shows which project (`POST /api/page`), so a project Claude Code opened is shown in a tab showing it or in a Home tab, and a tab showing another project is not switched.

## Shortcuts

The defaults are the same as in the macOS version and can be changed under App Settings > Shortcuts (saved in the browser's localStorage).
There are single-key operations that work only while the Canvas has focus (`s` settings, `a` artifacts, `l` logs, `f` fold, `c` duplicate, `0` fit all, `1` 100%, `g` group, `e` add a Draft and so on), and operations that work anywhere (⌘R run, ⌘. stop, ⌥⌘J chat, ⌥⌘L logs, ⌥⌘T timeline, ⌃` terminal and so on).
Outside macOS, use Ctrl in place of ⌘. Keys the browser reserves (⌘W, ⌘T, ⌘N, ⌘Q and the like) never reach the page even if assigned.
