# Web UI

The browser workbench supports editing and running Workflows, inspecting results and working with the Agent. It uses the same project and RPC contract as the macOS app. The host environment, rendering support and some shortcuts depend on how you run it. [Workbench operations](Usage.md) · [Platform behaviour](#the-tsugumi-command)
File paths and commands on this page are relative to the root of the repository.

The browser and Agent use the [project RPC](Protocol.md); review the [execution and data boundaries](security.md) for the chosen host.

Card names appear throughout the UI; exact identifiers remain in tool arguments and file paths. Editors retain text typed while a save is pending. Inline values use the revision from the start of the edit, and an unchanged value is not saved. **Reload External Changes** asks for approval when outside edits widen Card permissions; declining leaves the loaded project unchanged.

## The tsugumi command

Install the command using the [installation guide](INSTALL.md#using-it-in-the-browser-the-tsugumi-command). `tsugumi-hostd` serves the Web UI whenever it runs with a built page: run `tsugumi web` to start the host when none runs and open a page, or `tsugumi web <project folder>` to show that project. The command prints the page's URL and returns; hostd, project environments and core conversations remain resident. Stop a project's environment explicitly when its execution should end.

Showing is one hostd request (`ShowRequest` in `protocol/agent.fbs`) for the user and for Agents alike: a page that shows the project (a browser tab or a window of the Mac app, which then comes forward), or one on Home, is told to; when no page is free, hostd opens the Web UI with the system's opener (`open`, `xdg-open`, or the Windows browser in WSL). It never launches the desktop app: the app's windows are pages only while you have it open. The app carries the built page, so a hostd it starts serves the Web UI too. The default port is 8780; an occupied port is replaced by a free one. Use `tsugumi update` to install the latest stable distribution; a running host keeps its version until it stops.

Register a Linux worker under **Settings → Execution service** to run individual Code Cards remotely. [Remote execution](RemoteExecution.md)

**On a Mac**, the Web UI and the native app ask host-side `tsugumi-hostd` to manage project environments. Only hostd connects to the signed `tsugumi-vmhost` helper, through `protocol/vm_host.fbs`. [Environment ownership](features.md#1-architecture)

```text
Browser ── WebSocket /rpc ── loopback relay ── vsock ── tsugumid (one per project in the VM)
   └─ page and WebSocket /rpc/host, /rpc/agent ── tsugumi-hostd
                                                    └─ private VM RPC ── tsugumi-vmhost
```

The page supplies its origin in `EnvironmentRequest`; hostd applies them when starting the project daemon. The browser authenticates to that daemon with its project token. Agent requests run in hostd on the host. [RPC authentication](security.md#6-rpc-authentication)

The installed Mac command uses the release app's support directory by default:

| Data | Location and sharing |
| --- | --- |
| API keys | Stored by the shared hostd, the same keys as the app. [Credential handling](security.md#7-the-models-api-keys-agent) |
| VM, store, project control folders | `~/Library/Application Support/Tsugumi/`; override with `TSUGUMI_SUPPORT_DIR` |
| Recent projects | `recent-projects.json` under hostd's state directory; shared by clients of that service |
| Default project destination | `~/Documents/Tsugumi` |
| Settings, Skill folders, ChatGPT accounts and scripted guide progress | hostd's one `desktop` profile per host, shared by the app and every page |

The helper holds `vm/vm.lock`. The VM remains resident when pages or the app disconnect. Explicit Host runtime shutdown stops it. Use matching app and CLI versions with a shared support directory.

**On Linux**, hostd starts a native `tsugumid` per project; Cards use bubblewrap (`nix/cli.nix`). hostd's state defaults to `~/.local/share/tsugumi-web/host-core`; settings and saved provider keys are in hostd's owner-only store. The terminal uses a host shell through hostd's HostTerminal RPC; hostd owns its PTY and forwards input, resize and output.

**On Windows**, `tsugumi.cmd` runs the Linux distribution inside WSL2 (`cli/install.ps1`). hostd detects `WSL_DISTRO_NAME` and opens the Windows browser. WSL localhost forwarding exposes the Web UI and project ports to that browser.

### Running it on another machine, over SSH

You can run `tsugumi web --no-browser` on a Linux machine and use it from the browser at hand through SSH port forwarding.

Besides hostd's Web UI (port 8780), the page connects directly by WebSocket to the `tsugumid` of the open project. `tsugumid` listens on a free port of that machine's `127.0.0.1`, and the port is assigned when the environment starts and remains with its resident daemon. Forwarding only 8780 is therefore not enough to connect to a project: also forward the project's port shown on screen.

```bash
ssh -L 8780:127.0.0.1:8780 -L <port shown>:127.0.0.1:<port shown> <machine>
```

## Building and starting

How to run it from the repository (using hostd and this host's `tsugumid`).

Prerequisites: Nix (with flakes enabled), Node.js 22 or later to build the page, and a browser with WebGPU (Chrome / Edge / Safari 26 or later). Node is a build tool only; nothing runs on it.

```bash
nix develop --command bash -c 'cmake -S . -B build -G Ninja && ninja -C build tsugumid tsugumi-worker tsugumi-hostd tsugumi'
```

```bash
npm --prefix ui/web install
nix develop --command npm --prefix ui/web run gen
npm --prefix ui/web run build
```

```bash
build/cli/tsugumi web
```

The command finds `tsugumi-hostd`, `tsugumid`, the worker and the page (`ui/web/dist/public`) in the checkout and starts hostd with them; `--port` asks for a port and a project folder shows that project at once. Open the `http://127.0.0.1:8780/?key=…` it prints when the browser does not open.

`key` is a key made at each hostd start. hostd replaces it with a cookie only this page sends (HttpOnly, SameSite=Strict) and removes it from the address bar. The page's Host token (`POST /api/host/connect`, which also needs a dedicated header) and its `/rpc/host` and `/rpc/agent` WebSockets require this cookie and the page's own Origin; hostd then authenticates the binary Hello. Responses carry a CSP (`frame-ancestors 'none'`) and `nosniff`. Only the user's principal gets a URL with the key; the Claude Code plugin works as the external principal ([Security §7](security.md#the-agents-permission-boundary)).

| Environment variable | Meaning |
| --- | --- |
| `TSUGUMID` / `TSUGUMI_WORKER` | Project daemon and worker locations (defaults under `build/`) |
| `TSUGUMI_NIX_DIR` | The core's Nix files used to evaluate Cards (default `share/tsugumi/nix` beside the installed binaries). Cards obtain SDKs from their project’s [Nix search path](features.md#project-nix-search-path) |
| `TSUGUMI_HOSTD` / `TSUGUMI_HOST_STATE` | Host service binary and its state folder (defaults chosen by `cli/src/launch.cc`) |
| `TSUGUMI_WEB_ROOT` / `TSUGUMI_WEB_PORT` | The built page hostd serves and the port it asks for |
| `TSUGUMI_VM_X86=qemu` | The Mac's shared VM runs x86_64-linux Cards with qemu instead of Rosetta (by default Rosetta when it is installed; [Another system](features.md#another-system-x86_64-linux)). Also read by the desktop app |
| `TSUGUMI_VMHOST` / `TSUGUMI_VM_IMAGE` / `TSUGUMI_SUPPORT_DIR` | `tsugumi-vmhost`, the guest archive directory and the support folder when using the Mac's shared VM (set by `cli/tsugumi-macos`) |
| `TSUGUMI_WEB_DATA` | Linux: the folder holding hostd's state and native environments (default `~/.local/share/tsugumi-web`) |

The TypeScript bindings and `tsugumi.bfbs` are generated into `ui/web/shared/gen` by `npm run gen` (using `flatc` from the development shell) and are not in Git.

## Isolation (macOS host)

Starting the repository's daemon directly on a Mac uses Seatbelt. Review its [weaker guarantees and network limits](security.md#direct-macos-host) before running untrusted Cards. The installed Mac command uses the shared VM instead.

## Several projects

Each browser tab can open a different project (how this corresponds to the app's windows: [Several projects](Usage.md#several-projects)). ⌘O, demos and duplicates from a project's tab open in a new tab, and "Open Home in a New Tab" on the Canvas adds a Home tab. A browser cannot bring another tab to the front, though, so the same project can be open in two tabs (equal clients of the same `tsugumid`; a save in one shows in the other). Going back Home closes that project's windows, such as Card settings. If no other tab shows the project, its terminal tabs and chat observation close; hostd, the project daemon and active core conversations continue. Reopening the project reconnects to them.

hostd manages one `tsugumid` per project. Each page reports what it shows on its Host connection (`HostPageRequest`; the Mac app's main windows share one connection and each names its page), so a project the user or Claude Code shows goes to a tab or window showing it or to one on Home, and one showing another project is not switched.

## Shortcuts

The defaults are the same as in the macOS version and can be changed under App Settings > Shortcuts (saved in the browser's localStorage).
There are single-key operations that work only while the Canvas has focus (`s` settings, `a` artifacts, `l` logs, `f` fold, `c` duplicate, `0` fit all, `1` 100%, `g` group, `e` add a Draft and so on), and operations that work anywhere (⌘R run, ⌘. stop, ⌥⌘J chat, ⌥⌘L logs, ⌥⌘T timeline, ⌃` terminal and so on).
Outside macOS, use Ctrl in place of ⌘. Keys the browser reserves (⌘W, ⌘T, ⌘N, ⌘Q and the like) never reach the page even if assigned.
