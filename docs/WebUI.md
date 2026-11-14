# Web UI

The browser workbench supports editing and running Workflows, inspecting results and working with the Agent. It uses the same project and RPC contract as the macOS app. The host environment, rendering support and some shortcuts depend on how you run it. [Workbench operations](Usage.md) · [Platform behaviour](#the-tsugumi-command)
File paths and commands on this page are relative to the root of the repository.

The browser and Agent use the [project RPC](Protocol.md); review the [execution and data boundaries](security.md) for the chosen host.

## The tsugumi command

Install the command using the [installation guide](INSTALL.md#using-it-in-the-browser-the-tsugumi-command). Run `tsugumi` to start the Web host and open a browser, or `tsugumi <project folder>` to open that project. Ctrl-C stops the Web host; project environments and core conversations remain resident. Stop a project's environment explicitly when its execution should end.

If a server is running with the same data folder, the command asks it to open a page and exits. The default port is 8780; an occupied port is replaced by a free one. Use `tsugumi update` to install the latest stable distribution, then restart the Web host to load its new files. This does not restart resident Agent or project services.

Register a Linux worker under **Settings → Execution service** to run individual Code Cards remotely. [Remote execution](RemoteExecution.md)

**On a Mac**, the Web host and native app ask host-side `tsugumi-agentd` to manage project environments. Only agentd connects to the signed `tsugumi-vmhost` helper, through `protocol/vm_host.fbs`. [Environment ownership](features.md#1-architecture)

```text
Browser ── WebSocket /rpc ── loopback relay ── vsock ── tsugumid (one per project in the VM)
   └─ HTTP /api ── Web host ── Host / Agent RPC ── tsugumi-agentd
                                                   └─ private VM RPC ── tsugumi-vmhost
```

The Web host supplies its page origins in `EnvironmentRequest`; agentd applies them when starting the project daemon. The browser authenticates to that daemon with its project token. Agent requests run in agentd on the host. [RPC authentication](security.md#6-rpc-authentication)

The installed Mac command uses the release app's support directory by default:

| Data | Location and sharing |
| --- | --- |
| API keys | The same login Keychain items as the app; agentd keeps provisioned keys in memory. Web Keychain-save failures use agentd's private store. [Credential handling](security.md#7-the-models-api-keys-agent) |
| VM, store, project control folders | `~/Library/Application Support/Tsugumi/`; override with `TSUGUMI_SUPPORT_DIR` |
| Recent projects | `recent-projects.json` under agentd's state directory; shared by clients of that service |
| Default project destination | `~/Documents/Tsugumi` |
| Language, provider, model, effort, Fast, web search, permission preferences, version note mode | agentd's `desktop` settings profile, shared with the app |
| Skill folders and ChatGPT accounts | Separate `web:<Web data folder>` profile in agentd |
| Scripted guide progress | Separate desktop and Web sessions in agentd; shared across that client's windows or tabs |

The helper holds `vm/vm.lock`. The VM remains resident when the Web host or app disconnects. Explicit Host runtime shutdown stops it. Use matching app and CLI versions with a shared support directory.

**On Linux**, agentd starts a native `tsugumid` per project; Cards use bubblewrap (`nix/cli.nix`). Web data defaults to `~/.local/share/tsugumi-web`; settings and saved provider keys are in agentd's owner-only store. The terminal uses a host shell through `node-pty`.

**On Windows**, `tsugumi.cmd` runs the Linux distribution inside WSL2 (`cli/install.ps1`). The server detects `WSL_DISTRO_NAME` and opens the Windows browser. WSL localhost forwarding exposes the Web host and project ports to that browser.

### Running it on another machine, over SSH

You can run `tsugumi` on a Linux machine and use it from the browser at hand through SSH port forwarding.

Besides the server (port 8780), the page connects directly by WebSocket to the `tsugumid` of the open project. `tsugumid` listens on a free port of that machine's `127.0.0.1`, and the port is assigned when the environment starts and remains with its resident daemon. Forwarding only 8780 is therefore not enough to connect to a project: also forward the project's port shown on screen.

```bash
ssh -L 8780:127.0.0.1:8780 -L <port shown>:127.0.0.1:<port shown> <machine>
```

## Building and starting

How to run it from the repository (using agentd and this host's `tsugumid`).

Prerequisites: Nix (with flakes enabled), Node.js 22 or later, and a browser with WebGPU (Chrome / Edge / Safari 26 or later).

```bash
nix develop --command bash -c 'cmake -S . -B build -G Ninja && ninja -C build tsugumid tsugumi-worker tsugumi-agentd'
```

```bash
npm --prefix web install
nix develop --command npm --prefix web run gen
npm --prefix web run build
```

The terminal's `node-pty` is a native module. On macOS, a prebuilt one is in the distribution; on Linux it is built during `npm install` (needs a C++ compiler and Python). If it cannot be built, `npm install` and the Web UI still work, and opening the terminal shows the reason.

```bash
node web/dist/server.js --open
```

Open the `http://127.0.0.1:8780/?key=…` shown at startup (`--open` opens it automatically). `--port` sets the port, and a project folder as an argument opens that project at once. Build all three native targets above before starting the host.

`key` is a key made at each start. It is replaced by a cookie only this page sends (HttpOnly, SameSite=Strict) and disappears from the address bar. Browser requests to `/api/` (GET and event subscriptions included) require this cookie. Selected external-agent routes also accept the separate key described below. Calls that change something require a dedicated header as well. Responses carry a CSP (`frame-ancestors 'none'`) and `nosniff`.

The command and the [Claude Code plugin](claude-code.md) discover the host through `server.json` in the data folder. It contains the port, PID, build fingerprint and agent key (how far that key reaches: [Security §7](security.md#the-agents-permission-boundary)). The fingerprint covers the server bundle, its runtime location and native executables, including builds sharing a release version. Discovery and publication use one startup lock, so concurrent starts reuse the same host. An existing host from a different build is refused; stop it before starting the new build. A project argument is forwarded to the existing host when one is reused. A `server.json` without the current fields is rejected; there is no older-format fallback.

| Environment variable | Meaning |
| --- | --- |
| `TSUGUMID` / `TSUGUMI_WORKER` / `TSUGUMI_SDK_DIR` | Project daemon, worker and SDK locations (defaults under `build/` and `sdk/`) |
| `TSUGUMI_AGENTD` / `TSUGUMI_AGENT_STATE` / `TSUGUMI_AGENT_SCHEMA` | Agent service binary, state directory and generated `agent.bfbs` schema; defaults are selected by `web/server/coreAgent.ts` |
| `TSUGUMI_VM_X86=qemu` | The Mac's shared VM runs x86_64-linux Cards with qemu instead of Rosetta (by default Rosetta when it is installed; [Another system](features.md#another-system-x86_64-linux)). Also read by the desktop app |
| `TSUGUMI_VMHOST` / `TSUGUMI_VM_IMAGE` / `TSUGUMI_VM_SOURCE` / `TSUGUMI_SUPPORT_DIR` | `tsugumi-vmhost`, the boot disk, the source and the support folder when using the Mac's shared VM (set by `cli/tsugumi-macos`) |
| `TSUGUMI_WEB_DATA` | Web host data folder and profile identity (default on macOS: `~/Library/Application Support/Tsugumi/web`) |
| `ANTHROPIC_API_KEY` / `OPENAI_API_KEY` | Web-process API keys provisioned to agentd in memory; take precedence over saved keys |
| `TSUGUMI_CHATGPT_ISSUER` / `TSUGUMI_CHATGPT_API` | Where ChatGPT sign-in and plan requests go (for tests; by default `https://auth.openai.com` and `https://api.openai.com`) |

The TypeScript bindings and `tsugumi.bfbs` are generated into `web/src/gen` by `npm run gen` (using `flatc` from the development shell) and are not in Git.

## Isolation (macOS host)

Starting the repository's daemon directly on a Mac uses Seatbelt. Review its [weaker guarantees and network limits](security.md#direct-macos-host) before running untrusted Cards. The installed Mac command uses the shared VM instead.

## Several projects

Each browser tab can open a different project (how this corresponds to the app's windows: [Several projects](Usage.md#several-projects)). ⌘O, demos and duplicates from a project's tab open in a new tab, and "Open Home in a New Tab" on the Canvas adds a Home tab. A browser cannot bring another tab to the front, though, so the same project can be open in two tabs (equal clients of the same `tsugumid`; a save in one shows in the other). Going back Home closes that project's windows, such as Card settings. If no other tab shows the project, its terminal tabs and chat observation close; agentd, the project daemon and active core conversations continue. Reopening the project reconnects to them.

agentd manages one `tsugumid` per project. The Web host knows which tab shows which project (`POST /api/page`), so a project Claude Code opened is shown in a tab showing it or in a Home tab, and a tab showing another project is not switched.

## Shortcuts

The defaults are the same as in the macOS version and can be changed under App Settings > Shortcuts (saved in the browser's localStorage).
There are single-key operations that work only while the Canvas has focus (`s` settings, `a` artifacts, `l` logs, `f` fold, `c` duplicate, `0` fit all, `1` 100%, `g` group, `e` add a Draft and so on), and operations that work anywhere (⌘R run, ⌘. stop, ⌥⌘J chat, ⌥⌘L logs, ⌥⌘T timeline, ⌃` terminal and so on).
Outside macOS, use Ctrl in place of ⌘. Keys the browser reserves (⌘W, ⌘T, ⌘N, ⌘Q and the like) never reach the page even if assigned.
