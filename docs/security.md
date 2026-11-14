# Security and isolation

This document describes Tsugumi's four trust boundaries (what is isolated from what) and how secrets (the API keys of outside models and the RPC's authentication token) are handled.
Everything here is based on the current implementation (`core/src/vmd_main.cc`, `core/src/worker_proc.cc`, `core/src/server.cc`, `core/src/agent/`, `macOS/Sources/Launcher/`, `macOS/Sources/AgentClient/Provider.swift`, `web/server/coreAgent.ts`).

## Summary

### What Tsugumi takes on

A Tsugumi project is code to run and the definition of the environment it runs in. Opening and running a project is the user's decision to trust its contents, no different from running `make` in a repository you `git clone`d or executing a Jupyter notebook you received. Tsugumi's job is to run it as its author wrote it, so Tsugumi neither judges nor guarantees whether the project's author can be trusted. That is the boundary of Tsugumi's guarantees.

Inside it, Tsugumi takes on the following:

- **AI does not widen permissions without the user's approval.** A save in which the Agent gives a Card the network, running without isolation, a Mac folder or an external Nix source does not go through until the user approves (§7). For a tool you hand work to AI with, this is the boundary that matters most.
- **Closed by default.** By default a Card cannot reach the network, and cannot see the project's definitions, the authentication token or other Cards' work areas. It widens only when the project says so (§4).
- **The permissions written are visible.** Widened permissions are in readable files, `_card.json` and `tsugumi.json`, and appear on the Card's icons on the Canvas and among the Experimental items of the Card's settings. The `.tsugumi/` of a project from someone else is inspected before use.
- **The Mac and other projects are protected.** In the app (the shared VM), even if a project could not be trusted, its files and processes reach only that project and the folders explicitly shared with it (§2, §3).

The table below lists the limits of these guarantees. Details are in each section.

| Outside the guarantees | Why | Details |
| --- | --- | --- |
| Opening someone else's project, there is no confirmation for `network`, `sandbox: false` and `hostFolders` written in `_card.json`, or for `nixSources` in `tsugumi.json` | Opening a project is a decision to trust its contents ("What Tsugumi takes on" above). The permissions written are visible in the files and on the Cards, and only when the Agent adds one does it wait for approval | [§9](#9-known-limitations-and-cautions) |
| With the Agent, the code, logs and input and output values the Agent reads are sent to the model's provider | Having an outside model read something means sending it. Only what the Agent reads is sent, and whether to use it, and which provider, is the user's choice. Without the Agent, every operation can be done from the UI | [What is sent to the model](#what-is-sent-to-the-model) |
| With web search on, search terms go to the search service. It cannot be guaranteed that unpublished data stays out of them | Off by default; turning it on is the user's choice. While off, the model is not shown the search tool | [Web search](#web-search) |
| Isolation between projects is on one Linux kernel and does not prevent kernel vulnerabilities | The same isolation as containers; the Mac is separated by the VM. Do not open at the same time projects you want strongly separated | [§9](#9-known-limitations-and-cautions) |
| Even with `network: false`, Nix's fixed-output derivations can communicate at build time | How Nix works. What is fetched is checked against its hash and discarded if it does not match, so the content cannot change. What goes out is the request to the source the author wrote | [§5](#5-limits-on-nix-evaluation) |

## 1. The trust boundaries

There are four boundaries, from the innermost outwards.

| Boundary | Protects | Mechanism |
| --- | --- | --- |
| Card (worker) ⇄ tsugumid | The whole project, the authentication token, other Cards' work areas, the outside network (by default) | bubblewrap separating all namespaces, and dropping capabilities |
| project ⇄ project | Other projects' files, processes, run histories and build roots | Own uid, namespaces and cgroup (managed by `tsugumi-vmd`) |
| VM ⇄ macOS | All of the Mac's files and the home folder | `Virtualization.framework`. Only the open projects' folders and folders specified in Card definitions or the chat (§2) are shared |
| The RPC on macOS ⇄ other processes and the network | The authority to operate the core | Listening only on `127.0.0.1`, token authentication, checking the WebSocket's `Origin` |

## 2. VM ⇄ macOS

### What the VM is given

| Device | Contents | Writable |
| --- | --- | --- |
| rootfs | An erofs image bundled with the app (Linux kernel 6.12.115 and Nix, `macOS/Container/vm-boot.nix`). Tsugumi itself is built by Nix inside the VM from the bundled source (`macOS/Container/vm-system.nix`; dependencies fetched from cache.nixos.org with their signatures verified) | Read-only (the shared source is read-only too) |
| Store disk | `~/Library/Application Support/Tsugumi/vm/store.img` (the Nix store shared by all projects; for a development build from source, under `Tsugumi-dev/`, and likewise below) | Yes |
| Run-history disk | `~/Library/Application Support/Tsugumi/vm/history.img` (all projects' run histories, one directory per project) | Yes |
| virtiofs (per slot) | The open project's folder (`project`) and its control folder (`control`) | Yes |
| virtiofs (same slot, only when specified) | Mac folders specified in a Card's settings (`host-<tag>`) and Mac folders attached to the chat (`chat-<tag>`) | `host-` only with `write: true`; `chat-` read-only |
| virtiofs `rosetta` (only when Rosetta is installed) | Rosetta for Linux (`VZLinuxRosettaDirectoryShare`), for Cards with `"system": "x86_64-linux"`; registered with binfmt_misc by the VM's supervisor (`macOS/Container/vm-system.nix`) | Read-only |
| vsock | The control channel (1024) and the RPC relay (1025) | — |
| Network | NAT (`VZNATNetworkDeviceAttachment`) | — |

- **Of the Mac's files, the VM sees only the folder of each open project and its control folder, and the folders explicitly specified in that project's Card definitions or chat.** Neither the whole home folder nor other projects are shared. Stopping a project ends its slot's sharing; closing a UI leaves the resident environment running.
- There are two routes for sharing a Mac folder with the VM (`core/src/host/environment.cc`; the signed helper applies the resulting mount plan).
  - **A Card's "Allow access to folders of the host"** (`hostFolders` in `_card.json`, Experimental): folders written in the Card definitions of the project (and its `externals`) are shared into that project's slot when the environment starts. Those without write permission (`write: true`) are read-only. **This sharing is decided by what is written in the definition files alone, with no confirmation on opening** (a save the Agent makes waits for approval; §7, §9).
  - **Attaching a folder to the chat**: a folder for the Agent to read is shared read-only with that project.
- Each project is given its own virtiofs device (`tsugumi-<slot>`) for sharing, so replacing one project's shares does not affect others.
- The only entitlement given to the app is `com.apple.security.virtualization` (`macOS/Resources/Tsugumi.entitlements`).
- The signed `tsugumi-vmhost` helper holds the file lock (`flock`) on `vm/vm.lock`; agentd requests VM operations through the private helper contract.

### The VM's network

The VM's NAT exists **so that `nix-daemon` can fetch the binary cache from `cache.nixos.org` and download the pinned nixpkgs archives**.
Cards themselves are cut off from the network namespace by default and cannot communicate outside. Only a Card with "Allow network access" turned on in its settings (`network: true` in `_card.json`, Experimental) can communicate outside through this NAT (§4).

## 3. project ⇄ project (inside the VM)

The managing process inside the VM, `tsugumi-vmd`, starts each project's `tsugumid` under these isolation conditions (`core/src/vmd_main.cc`).

- **Own uid**: each project is assigned a uid of its own (from 20001, recorded in `/persist/tsugumi/uids`), and root is given up with `setgroups(0)` → `setgid` → `setuid` before starting.
- **Namespaces**: `CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS` are separated.
- **Visible files**: only the paths below are bind-mounted in the mount namespace; the parent of all slots (`/mnt`), the whole store disk (`/persist`) and the whole run-history disk of all projects (`/history-disk`) are all unmounted.
  - `/project` … its own project folder
  - `/control` … its own control folder
  - `/history` … its own run-history folder (on the run-history disk, `nosuid,nodev`)
  - `/scratch` … its own disk-backed invocation scratch (`/persist/run-scratch/<id>`, owned by the project uid, mode 0700). The core validates scratch ownership and private parents, rejects symbolic links and, on Linux, rejects tmpfs/ramfs (`core/src/run_scratch.cc`).
  - its own `/tmp` and `/run/tsugumi` (`tmpfs`)
  - `/run/tsugumi/rpc` … the sockets `tsugumid` listens on (WebSocket, Unix socket). They actually live in `/run/tsugumi-rpc/<id>` (a 0700 folder only that project's uid can enter), and `tsugumi-vmd` relays to them. There are no ports of any project on the VM's loopback (a Card allowed the network shares the VM's network, so a port there would let it reach another project's `tsugumid`).
  - `/run/tsugumi/host/<tag>`, `/run/tsugumi/chat/<tag>` … Mac folders shared with that project (§2, only when specified)
  - `/nix` … an overlay mount (the shared store is readable from every project; writes go only through `nix-daemon`)
- **cgroup**: placed at `/sys/fs/cgroup/tsugumi/<id>`. Stopping a project ends exactly the processes in that cgroup with `cgroup.kill`, without affecting other projects.
- **GC roots**: `/nix/var/nix/gcroots/tsugumi/projects/<id>/` is created by `tsugumi-vmd` as root, and the parent directory is not writable from projects, so another project cannot take it over by creating the directory first (checked in `test_shared_vm.py`).
- **Kernel**: Linux 6.12 LTS (`macOS/Container/kernel.nix`) without io_uring, and with unprivileged BPF off by default. `/init` uses sysctls to restrict reading kernel addresses and logs, ptrace (descendants only) and planting links and files in shared sticky folders (Yama is enabled; BPF JIT and userfaultfd are not built in).
- **Terminal**: opening the terminal inside the VM enters the project's namespaces with `setns`, joins the same cgroup and drops to the project's uid before starting `bash`. Stopping the project environment ends its terminals too.

### What projects share

- `nix-daemon` and `/nix/store` are shared. Every path in the store is identified by the hash of its inputs, and builds happen in `nix-daemon`'s sandbox (`sandbox = true`), so one project cannot rewrite another's build results. By Nix's design, however, **which paths exist in the store is visible to every project**. Do not put secrets in the Nix store.

The human-authenticated Host environment service in `tsugumi-agentd` launches native project processes and grants their execution-broker capability before reporting readiness. It authenticates the project endpoint and checks its project/server identity when reattaching a resident process. Stop and observed process death revoke that capability and remove the Agent's endpoint registration. Attached-folder withdrawal replaces the control file immediately. The native service never archives history unless the person requests it with the archive flag.

Only agentd talks to the signed Mac helper, over the owner-only Unix socket using `protocol/vm_host.fbs` (bounded length-prefixed FlatBuffers). The helper retains Virtualization.framework operations, virtiofs shares, vsock relays and the terminal descriptor bridge; readiness, project identity registration, mount selection, broker authorization and lifecycle decisions belong to agentd. The helper stores no preferences and makes no Agent requests. The VM and project processes remain resident after UI disconnection; closing a page does not revoke an environment's execution authorization.

## 4. Card (worker) ⇄ tsugumid

Each Card's code is further run inside a `bubblewrap` sandbox (`core/src/worker_proc.cc`).

### `sandbox: true` (the default)

- **All namespaces are separated** (user, mount, PID, network, IPC, UTS, cgroup). The only network interface is loopback (`lo`). Only for a Card with `network: true` is the network namespace left shared (`--share-net`), with the name-resolution settings and the CA certificate file mounted read-only.
- **All Linux capabilities are dropped** (`--cap-drop ALL`).
- **Visible files** are limited to `/nix/store` (read-only), for a Card built for another system the folder of its emulator when that is outside `/nix/store` (Rosetta's `/run/rosetta`, read-only), the worker's executable (for a package Card of another system, the worker its build made for that system, in `/nix/store`; when it is outside `/nix/store` in a development build, that file alone, read-only), the project's `runs` and saved inputs (read-only), a temporary work area for that worker alone (writable), and the Mac folders that Card specified in `hostFolders` (`/host/<name>`, writable only with `write: true`). The project's definition documents and source code, the control folder, the authentication token, other Cards' work areas, host folders other Cards specified and folders attached to the chat are not visible.
- **The environment variables are rebuilt from empty** (`PATH=/var/empty`, `LANG=C.UTF-8` and only the variables Tsugumi passes); nothing is inherited from `tsugumid` or the host.
- **Passing outputs**: each invocation's output directory is passed as a file descriptor (FD) with `SCM_RIGHTS` and accessed inside the worker as `/proc/self/fd/900`. After the run, the core maps it back to its real path and validates it.
- **Read-only inputs**: inputs are protected with read-only bind mounts rather than file descriptors.
- **Where `bubblewrap` is not available, a `sandbox: true` Card is refused at once without running** (`permission_denied`). Nothing ever runs without isolation implicitly.
- The exception is starting the [Web UI](WebUI.md) from the repository and running `tsugumid` directly on a Mac host (the `tsugumi` command uses the shared VM, so this does not apply to it). There, `sandbox-exec` (Seatbelt) isolates instead of bubblewrap (no network; reading only `/nix/store`, the run's and inputs' areas and system libraries; writing only the worker's own area and the run's output folder), and without `sandbox-exec` it is refused in the same way. Even a `network: true` Card is kept away from this machine's own addresses, Unix sockets and most Mach services. The details, and the paths that cannot be closed, are in [Direct macOS host](#direct-macos-host).

### `sandbox: false`

A setting that relaxes only the restrictions on seeing the file system. Where bubblewrap is available (the shared VM, the `tsugumi` command on Linux), these protections remain (on a `tsugumid` run directly on a Mac host without bubblewrap, a `sandbox: false` Card runs without isolation, with the user's permissions; §9):

- The user, PID, **network** and IPC namespaces are separated (no outside communication unless `network: true` is also set, and all descendant processes are reaped when the worker ends).
- The control folder and the `.tsugumi` directory are hidden behind empty `tmpfs` mounts. virtiofs presents every file as owned by the process looking at it, so ordinary file permissions (mode) alone cannot protect the authentication token.
- Files in the VM are accessible with the project's uid, so the Mac folders shared with that project (`/run/tsugumi/host/`, `/run/tsugumi/chat/`; §3) become readable too, including those other Cards specified and those attached to the chat.

### Direct macOS host

The repository's development server can run `tsugumid` directly on a Mac with Seatbelt (`core/src/worker_proc.cc`). This does not separate uids, and `stat` can reveal metadata outside the readable paths. Shared host-folder grants require the VM; a `sandbox: false` Card runs with the host user's permissions.

Even with network access, Seatbelt denies this machine's loopback, LAN/VPN addresses and Unix sockets, except the mDNSResponder socket needed for DNS. That exception also permits DNS-SD browsing/advertising. Certificate validation uses `com.apple.trustd.agent`; other Mach services, system proxy settings and user lookup are unavailable.

Do not grant untrusted Cards network access when these unavoidable limits matter: LAN/VPN peers remain reachable, UDP broadcasts/multicasts can reach this host, and a Card may open a listening socket. The shared VM is the installed Mac command's execution route.

### Showing Card views (line charts, Plotly, Vega-Lite and Card-owned modules)

Card views run in the shared isolated host (`ui/modules`). Built-in views, including the WebGPU line chart and the SDK-pinned Plotly/Vega libraries, are fetched from the core through RPC just like Card-owned modules. Run data are supplied separately as configuration and binary blobs.

The core accepts only Nix store module identities. A shared entry must resolve to a canonical store `index.js`, and the Card's Wasm must remain inside its own output directory (`core/src/module_source.cc`).

- Web UI: `/modules/viewer.html` is put in an iframe. Its response's CSP is `sandbox allow-scripts` (without `allow-same-origin`), so the component runs in an opaque origin and can use neither cookies nor `/api/`. With `default-src 'none'` and `connect-src 'none'` it loads nothing and connects nowhere. With `frame-src 'none'` the Card's inner frame (a `srcdoc` document) cannot navigate itself to any URL either. Its script is embedded in the page, and the library code is handed over by the parent page with postMessage. This is the only route that may be embedded (`frame-ancestors 'self'`); every other response forbids embedding.
- macOS app: the trusted host is loaded as HTML in a nonpersistent WKWebView without a file read grant. Card code runs in its opaque inner frame. Content rules block network/file resources, a navigation delegate denies external navigation, and native message handlers reject messages from subframes.
- In both, Vega's data loading (`data.url`) is refused by the loader. The SDK sends charts with their data inside the spec.

## 5. Limits on Nix evaluation

A Card's `default.nix` may contain any Nix expression, but evaluation enforces these limits (`core/src/nix_eval.cc`, `macOS/Container/vm-boot.nix`):

- `restrictEval = true`: no files can be read outside the allowed paths (the Card's source, the SDK and the upstream definitions it may refer to).
- `enableImportFromDerivation = false`: running builds or commands during evaluation of a Nix expression (IFD) is forbidden.
- `allowedUris = ["https://github.com/NixOS/nixpkgs/archive/"]`: the URLs that can be fetched over the network are limited to the official nixpkgs archives (`NIX_PATH` is empty too). The Cards' evaluator has no `builtins.getFlake`.
- Flakes a project declares in `nixSources` of `tsugumi.json` are evaluated by `tsugumid` with an evaluator separate from the Cards' ("External Nix sources" in [Features](features.md), [test_nix_sources.py](../tests/core/integration/test_nix_sources.py)). This evaluator is the same pure evaluation as `nix build`, with IFD forbidden. Declarations are limited to ones pinned by commit and hash. Since they are evaluated outside the Cards' isolation, declaring one is a decision to trust the flake's expressions. Only the packages' derivations reach the Card.
- Builds run in `nix-daemon`'s sandbox (`sandbox = true`, `build-users-group = nixbld`), and with `trusted-users = root` only, a project's uid cannot rewrite substituters or Nix settings. Custom [binary cache connections](BinaryCaches.md) are human application settings outside the project. Their keys are trusted across the shared VM; private-cache credentials stay in `0600` application files and a read-only boot snapshot hidden by project mount namespaces.
- The default binary caches are `https://cache.nixos.org` and `https://tsugumi.cachix.org`, with their signing keys pinned. Additional caches require an explicit application setting or host administrator configuration.
- **Fixed-output derivations (`fetchurl` and the like, whose content is pinned by hash) can communicate outside at build time.** That is how Nix works, and it is separate from a Card's `network` setting (the `default.nix` of a `network: false` Card can still fetch data). What is fetched is discarded unless it matches the hash, but the request to the source itself (including any values in the URL) goes out.

## 6. RPC authentication

### Where it listens

- Inside the VM, `tsugumid` opens no TCP port; it listens for WebSockets on the Unix socket `/run/tsugumi/rpc/s` (`--listen-unix-ws`, `core/src/vmd_main.cc`). It actually lives in `/run/tsugumi-rpc/<id>`, a 0700 folder only that project's uid can enter (§3).
- To the Mac, it is **relayed through vsock only to the host's `127.0.0.1:<free port>`** (`LoopbackBridge`; on vsock 1025, `tsugumi-vmd` connects to the socket by project ID). It is not exposed to the LAN or outside networks.
- With no ports on the VM's loopback, there is no route to `tsugumid` from another project or from a Card allowed the network.
- The Mac's `tsugumi` command uses this route too (relayed by `tsugumi-vmhost`). In a Web UI started from the repository, and with the `tsugumi` command on Linux, `tsugumid` runs directly on the host and listens with a WebSocket on `127.0.0.1` ([Web UI](WebUI.md)).
- Unix domain sockets, when used, are created with `umask 0177` (permissions `0600`). The app's standard route to `tsugumid` is the WebSocket, and token authentication is always required.
- The host-side `tsugumi-agentd` has separate Agent (`protocol/agent.fbs`) and Host (`protocol/host.fbs`) listeners: Unix sockets and loopback WebSockets using `tsugumi.agent.v1` and `tsugumi.host.v1`. The app connects through the shared C ABI byte transport; the Web host connects directly with generated TypeScript bindings. The browser retains its HTTP/SSE API and never receives a host credential. Host rejects the external-agent token at Hello. `HelloRequest` authenticates distinct user and external-agent principals; only the user principal can register project endpoints or credentials and resolve or revoke grants. Its `agent.ready` file holds the tokens (0600, in a 0700 state directory).

### The authentication token

- At each start, `tsugumid` generates a 32-byte cryptographically random token (`crypto::random_token(32)`) and writes it **only to `server.ready` in the control folder (permissions `0600`, written atomically)**. The control folder itself is protected with `0700`.
- A client presents the token in its first `SessionHello` message. It is compared in constant time (`crypto::equal_secret`), and on a mismatch the connection is closed with `unauthenticated`.
- The token never appears in URLs or logs (the WebSocket URL is just `ws://127.0.0.1:<port>/rpc`), and the app keeps it only in memory.
- Under "Connections" in the settings you can copy the URL to connect to and the file path of `server.ready`, but the token's string itself is never shown on screen.

### Protecting the WebSocket

- The endpoint accepts only `GET /rpc` (anything else is 404) and requires `Sec-WebSocket-Protocol: tsugumi.rpc.v1`.
- A connection with an `Origin` header is checked against the allow list (`--allow-origin`; agentd applies the origins supplied by the client starting the environment) and refused with 403 on a mismatch. Ordinary web browsers always send an `Origin` header, so **a browser that opened a malicious web page cannot connect to Tsugumi on `127.0.0.1`** (preventing DNS rebinding and Cross-Site WebSocket Hijacking). Native clients without an `Origin` header (the app itself, the Agent, test scripts) authenticate with the token.
- The connection is closed at once on receiving an unmasked client frame, a frame with reserved bits set, a frame over the size limit, or a FlatBuffers message with an unknown union type or an invalid length (checked in `test_transport.py` and `test_contract.py`).

### Signed paging cursors

Paging cursors, for reading history and the like, carry a signature like an HMAC with a secret key generated at each start (the first 32 hex digits of `sha256(secret + payload)`), so a client cannot tamper with a cursor to read data out of range.

## 7. The model's API keys (Agent)

- On macOS, the app stores API keys in the user's **login keychain** (`macOS/Sources/AgentClient/Provider.swift`). The service name is `app.tsugumi.agent`, and the account name is the provider's name (`Anthropic` / `OpenAI`). The app and the Web host use the same items.
  - Login keychain items are not synced to iCloud Keychain. They can be read while the login keychain is unlocked (normally, while logged in).
  - An item's access list trusts the program that saved it. When another program, including `security`, tries to read an app-saved key, macOS asks the user. An ad hoc signed app may ask after each rebuild; signing development builds with `TSUGUMI_CODESIGN_IDENTITY` gives the app a stable identity ([Releasing](../RELEASING.md)).
  - The Web host reads and writes through `/usr/bin/security`. An item saved by the Web UI trusts `security`, so any program running as this user can read it without confirmation with `security find-generic-password`. When using both UIs, registering the key from the app's settings is recommended.
  - The data protection keychain (`kSecUseDataProtectionKeychain`) is not used. It requires an entitlement granted by a provisioning profile, which an ad hoc signed build cannot have (`errSecMissingEntitlement`).
- Platform adapters hand keys to `tsugumi-agentd` through the human-authenticated `ProviderSetRequest` with `ephemeral: true`. Keychain-backed keys live only in memory in agentd, are never written to its database, and are never returned by an RPC. Model selections contain an opaque memory-profile reference. If that profile is missing after a restart, it cannot fall back to a disk-stored key; the platform must provision it again.
- Outside macOS, or when the Web host cannot save a key in the Keychain, agentd stores it in `agent-core/host-settings.sqlite` (0600) within its owner-only state directory (0700). The native app reports a Keychain save error rather than saving the key to disk. Environment keys take precedence for their Web process, use the same memory-only provisioning, and do not overwrite a saved Keychain item or stored provider key.
- When the native model menu refetches a model list, it uses `Keychain.loadWithoutAsking`: a key that would require confirmation is not read, and the saved catalog remains visible. Chat requests and the settings screen's explicit fetch may ask. The Web host caches both successful and refused Keychain reads for the process lifetime, rereading only when the item's attributes change or the user saves or deletes it. Attribute-only availability checks never read a secret. Reading a stored catalog requires no key; an empty refresh retains it.
- Settings and provider interpretation belong to agentd. Revisioned updates reject stale edits, and settings contain no credentials. The Mac app and Mac Web host share desktop settings; ChatGPT accounts have separate app and Web profiles. The settings screen registers or deletes a key and cannot display a saved key.
- libcurl sends model credentials in the HTTPS header (`x-api-key` / `Authorization: Bearer`) without a cookie engine or disk response cache. Keys and OAuth tokens are excluded from settings, conversation replies, events and logs, and are never passed to the project's `tsugumid`, Linux VM or Card workers. Provider errors are scrubbed before crossing that boundary.
- Skill discovery returns metadata only. After a separate Read grant, agentd reads selected Skill files beneath the registered project or attached-folder root, including files outside the project's document index. Symlinks must resolve within that root and the subsequent bounded read refuses symlink replacement.

### The ChatGPT plan (Sign in with ChatGPT)

Instead of an API key, the Agent can use the user's ChatGPT (Plus, Pro) plan. This follows the [procedure OpenAI publishes for open-source tools](https://developers.openai.com/siwc/token-sharing-open-source) (ChatGPT plan usage) (`core/src/host/settings.cc` for OAuth and the callback; `core/src/agent/host.cc` and `provider.cc` for token refresh and revocation).

- Sign-in happens in the default browser. Tsugumi waits for `/auth/callback` on a free port of `127.0.0.1` and closes it as soon as the result arrives. Each time it uses a new `state`, `nonce` and PKCE (S256), and a response whose `state` does not match is discarded. No client secret and no developer API key are used (the first time, Tsugumi registers itself with `dynamic_agent_client` and uses the issued `client_id` for later sign-ins).
- The ID token's RS256 signature is checked against OpenAI's JWKS (`https://auth.openai.com/.well-known/jwks.json`), and its issuer, audience (the issued `client_id`), expiry and `nonce` are checked. Signing back in to a previous account, it also checks that it is the same account (`sub`). If permission to use the plan (`chatgpt.tokens.use.direct`) was not granted, the Agent does not use the plan and suggests granting it again or using an API key.
- The host ID (`ext_agent_host_id`) is a `urn:uuid:` (UUIDv4) made before the first sign-in, containing nothing that identifies the user.
- agentd stores host IDs, registrations and account metadata in `host-settings.sqlite` and OAuth credentials in `agent.sqlite`, both 0600 under the same owner-only state directory. The Mac app's account profile and each Web host's account profile are separate: signing in in the app does not sign in the Web host.
- `SignInBeginRequest` returns an attempt ID and a browser URL. Clients open the browser and poll `SignInStatusRequest`; `SignInActionRequest` cancels, selects an account, records the welcome message or signs out. PKCE, state, nonce, code exchange, JWKS verification and the loopback callback all run in agentd. Responses contain account status, never tokens or the PKCE verifier. Sign in with a browser on the server's machine so it can reach `127.0.0.1`.
- The Agent service serializes refresh and revocation, persists rotated tokens, and refreshes within five minutes of expiry or after a refused stale token. A refresh failure is reported without deleting credentials. Signing out attempts revocation and deletes the local credential even if the remote revocation cannot be confirmed; the UI reports that outcome. Registration metadata and the host ID remain.
- Access tokens go only in the model request header to the Responses API and model-list endpoint; refresh tokens go only to the issuer's token/revocation endpoints. These operations are absent from model tools and conversation events.
- Requests on the plan are sent with `store: false` (asking that the conversation's state not be stored; how data is handled at the destination follows OpenAI's policies). At a usage limit it stops without retrying, and never silently switches to another way of paying.

### What is sent to the model

When the Agent is used, the following is sent to the connected model provider (Anthropic / OpenAI; the ChatGPT plan is OpenAI too):
- The text of the chat conversation
- Information on Cards and visualisations added to the chat as references
- What the Agent read through its tools (the RPC): Card definitions, source code, run logs, input and output values, and the contents of files read with `AttachedFolderRead` from folders attached to the chat
- When asking the guide's thrush a question, and during a demo's guide: the question or tour message, and what the guide reads through authorized tools. The UI sends project references and selected Skill names; the Agent service reads their contents after authorization. The guide has no tools to save, run or stop, and does not change the project. While not connected to a model, the thrush answers from its script and sends nothing.
- With web search turned on under "API" in the settings: the search terms the Agent writes are passed on by the provider to its search service ([Web search](#web-search)).

- With "Version summaries" on (off by default; [Versions](Versions.md#version-summaries)): the diffs of code and settings changed in each version, the commit message and the names of the Cards that changed. Data files are not sent.

**Note that using the Agent sends part of the code, logs, input and output data and the contents of attached folders in the project to an outside model provider.** Before using the Agent on a project containing unpublished data or confidential information, check that it may be sent to the provider.

Conversely, **without the Agent, with "Version summaries" off and with no Card that has `network: true`, the project's content and data stay in the local execution environment, provided all Cards use the local execution service** (apart from Nix fetching packages). A Card allowed `network: true` can send the data it can access (its inputs, the contents of host folders specified for it and so on) to the outside network.

### Web search

A setting that lets the Agent search the web at its own discretion ("Web search" under "API" in the settings). **Off by default**; while off, the model is not shown the tool, and it is in neither requests nor rules.

- **The provider does the search.** It uses Anthropic's `web_search` and OpenAI's `web_search`; Tsugumi does not connect to a search service, and no new API key is needed. Search terms are handled by the provider's data policies. If search is disabled in the connected account's settings, the request fails (organisation settings in Anthropic's Console; the ChatGPT plan's model and workspace policies).
- **Search terms go outside, with the risk of unpublished data in them.** The model writes the search terms, and Tsugumi does not inspect them. The model's rules tell it not to put unpublished data, results, names, paths or code of files and Cards in search terms, but **this cannot be relied on to prevent it**. What the Agent has read may end up in a search term. For projects with unpublished research, decide whether to turn it on considering whether it may reach not only the provider but also the search service.
- **Searches are kept.** Search terms and the URLs found are kept, like other tools, in the chat's rows and the conversation's record (`tools`, `messages`). Even if a response fails and is discarded, the record of the search stays (the terms have already gone out).
- **Search results are outside text.** They may contain instructions to the Agent. The model's rules tell it not to treat them as instructions, but this is not a guarantee either. That a save widening a Card's permissions does not go through without the user's approval ([The Agent's permission boundary](#the-agents-permission-boundary)) does not change with search.
- **Helpers and the guide inherit the provider's web-search setting too.** Conversation compaction disables search for its summary request.
- **Counts and charges.** With Anthropic, searches are limited to five per turn; OpenAI has no such limit. Search may be charged separately from tokens (check the provider's pricing).
- With Anthropic, the basic version (`web_search_20250305`) is used: the newer version with dynamic filtering runs code on the provider's side and is not covered by zero data retention. The provider's opaque search-result blocks are kept in the Agent service's conversation record (`agent.sqlite`) and sent back to the provider in the next request. After the setting is turned off, they are not sent in that form; the search terms and URLs are sent as text.

### The Agent's permission boundary

- The only operations the Agent can perform are the published Agent and project RPC tools; it cannot operate the UI or the Mac's file system directly. To `tsugumid` it is one client equal to the UI, going through the same revision checks and conflict detection.
- Tool calls that change something carry a unique `op_id`, and when a response is cut off the applied state is confirmed with `OperationGet`, so blind retries never apply anything twice.
- Arguments the model wrote are checked by the Agent and `tsugumid`, and a tool call that is not a JSON object, or from a response cut off at the output limit, is not run.
- **A Card's permissions belong to the user.** When an Agent's save newly gives a Card any of the following, `tsugumid` does not save it and returns `approval_required`, listing each permission that widens (Cards inside a Card in Card included; `core/src/permissions.cc`):
  - the network (`network: true`)
  - running without isolation (`sandbox: false`)
  - Mac folders (`hostFolders`, reading and writing)
  - external Nix sources (`nixSources` in `tsugumi.json`)
  - switching to a registered remote execution service (`executionService`)

  The approval works like this:
  - The Agent service requires a `CardPermissions` grant, resolved by the user through Agent RPC. It consumes this grant for one widening; the UI presents the request rather than deciding the permission rule.
  - The approval is sent in a field the Agent service fills (`approved_permissions`); model-supplied approval and origin fields are removed.
  - If not allowed, nothing is saved, and the Agent is told to explain the reason to the user.
  - When the user changes it themselves in the Card's settings, nothing is asked. A Card on the Canvas shows the permissions it has as icons.
- **Shell commands need a separate Shell grant.** `project_shell` is available only when the isolated project launcher enables it, and requires Read, Write, Run and Shell access. It runs as the project user in the VM, with access to the project, its shared folders and the VM network. This is broader than per-Card permissions: a shell can edit documents directly. The model cannot grant itself Shell access. Commands and results are kept in the conversation; bounded output, deadlines, cancellation and durable operation receipts are enforced by core (`tests/core/test_project_shell.py`). An ordinary host daemon rejects the RPC instead of falling back to a host shell.
- **The Agent can write a Card's comments too.** This is not refused, but `tsugumid` records who wrote, changed and resolved each comment from the save's `origin`, so the model cannot pose as the user's comment (`core/src/comments.cc`, [test_card_comments.py](../tests/core/test_card_comments.py)). Saves that follow comments go through the same approval as above.
- **The Claude Code plugin** ([Using it from Claude Code](claude-code.md)) operates only through the Web UI's server.
  - The server forwards tools to `tsugumi-agentd` as an external-agent principal, with the same core-owned Read, Write, Run and CardPermissions checks as the chat. Pending access requests are presented on the Web UI's page; a disconnected page does not cancel a core conversation. Attached-folder reads require a user-registered folder and its own Read grant.
  - The plugin's MCP server finds the server through `server.json`, which the server writes in the data folder (the port, PID, build fingerprint and a per-start key for agents; 0600, deleted on exit).
  - That key reaches only the seven routes the plugin and a second run of the `tsugumi` command (which has the running server open a page) use (`/api/host/identify`, `/api/project/open`, `/api/project/create`, `/api/agent/tools`, `/api/agent/open`, `/api/agent/call`, `/api/page/open`; `AGENT_ROUTES` in `web/server/paths.ts`). It does not reach answers to approvals (`/api/chat/permission`), event subscriptions, the terminal, file listings or settings (`tests/python/test_claude_code_plugin.py`).
  - The page's key (the key in the URL shown at startup) is not written to `server.json`. To open a page, the server opens the browser itself.
  - What this boundary protects is **operations through Tsugumi's tools**. Claude Code also has a shell and file operations, so with the same permissions as the user it can rewrite the project's files (`_card.json` and the like) directly and read what the user can read (such as the URL in the log `server.log` of a server Claude Code started). Stopping that is up to Claude Code's own permission settings, not Tsugumi.
- `attached_folder_read` requires a folder registered by the user and an active Read grant for that resource, checked in `core/src/agent/host.cc` before the project RPC is called. Removing a folder from the UI's attachment list is separate from revoking its Agent access grant; agentd updates the attached-folder control file and withdraws the corresponding VM share when the folder is removed.

## 8. Where data is kept, and how to delete it

| Data | Where | Deleting and managing |
| --- | --- | --- |
| The project's documents and sources | The project folder | Manage in Finder or the like (Tsugumi never deletes the folder itself) |
| Run history (primary) | The VM's run-history disk `vm/history.img` (`/history-disk/<id>/` inside the VM) | "Delete…" under the project's Run History settings, or "Run history in the VM" in the settings |
| Run history (copy in the project) | `.tsugumi/runs.sqlite`, `.tsugumi/runs/` | "Delete…" under Run History, or stop writing it with `saveRunHistory = false` |
| Nix store, build roots | `~/Library/Application Support/Tsugumi/vm/store.img` | "Delete" under Build Cache and "Clean shared store" |
| Control folders (token, origins, attached folders) | `~/Library/Application Support/Tsugumi/environments/<id>/control/` | "Reset all environments" |
| API keys | macOS login Keychain; agentd's owner-only `host-settings.sqlite` for non-Mac/Web save fallback; Web environment keys in memory | Settings > API registers or deletes a key |
| Agent conversations, events, receipts, grants and provisioned credentials | `agent-core/agent.sqlite` under the Mac support folder (`Tsugumi/` or `Tsugumi-dev/`), or under the Web data folder on Linux (`TSUGUMI_AGENT_STATE` overrides it) | Conversation deletion goes through Agent RPC |
| ChatGPT account metadata and Skill folder settings | agentd's `host-settings.sqlite`, scoped separately for the app and each Web host | Settings > API / Skills |

"Reset all environments" in the settings stops the VM and deletes `vm/` (except the run-history disk `vm/history.img`) and `environments/`. The documents, artifacts and `.tsugumi/runs.sqlite` in project folders, and the run-history disk of the VM, are kept.

## 9. Known limitations and cautions

Most of these spell out, as practical cautions, what lies outside the boundary in the [summary](#what-tsugumi-takes-on) (whether to trust a project's contents).

- **Check the permissions written in a project you received before running it.** `network`, `sandbox: false` and `hostFolders` (especially `write: true`) in `_card.json`, and `nixSources` in `tsugumi.json`, take effect without confirmation both on opening and at run time (only saves the Agent makes wait for approval; §7). Look at the Cards' icons and the Experimental items of the Cards' settings. A `sandbox: false` Card can read every Mac folder shared with that project (§4).
- **Projects share one Linux kernel.** An exploited kernel vulnerability could reach other projects, so do not open at the same time projects you want strongly separated (§3).
- **A Web UI run directly on a Mac host from the repository has weaker isolation.** A `sandbox: false` Card runs without isolation with the user's permissions (`network: false` does not apply either), and a `network: true` Card can reach devices on the same LAN or VPN ([Direct macOS host](#direct-macos-host)). The `tsugumi` command uses the shared VM, so this does not apply to it.
- **Releases are signed ad hoc, without Apple's notarisation.** GitHub Actions builds them from this repository's tags (`.github/workflows/release.yml`), and automatic updates are installed after their checksum, code signature and more are checked ("Automatic updates" in [Releasing](../RELEASING.md)). The correctness of updates rests on trusting the people who can write to this repository and its GitHub Releases.

## Remote worker boundary

A registered execution host receives the Card's inputs and derivation closure. Choose a host trusted with those data. It builds and runs the invocation; it does not evaluate Card definitions.

All worker connections require TLS 1.3 and mutual Ed25519 authentication. agentd verifies the worker's complete SPKI against the registered pin before sending a request. The worker requires a client certificate and checks its SPKI against the owned `authorized_keys` file at each handshake. CA and hostname trust are replaced by these explicit pins; session resumption, tickets and early data are disabled. Removing a key refuses subsequent handshakes; an authenticated request may finish. Private keys stay with their owner and are absent from Host RPC.

Host-side agentd owns execution registrations and authorization. It derives a project scope from device, inode and birth time and replaces caller-supplied scopes. Copied or replaced directories get different scopes; a filesystem without birth time cannot use remote execution. The project's local broker capability is separate from worker authentication and never reaches the worker. Explicit project stop revokes it; UI disconnection and agentd restart preserve it. Replacing a registration invalidates its capabilities before another worker connection.

Use [Remote execution](RemoteExecution.md) for registration, key storage, recovery and journal/scratch management. Run-history deletion does not delete remote job scratch.
