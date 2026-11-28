# Security and isolation

Tsugumi separates the Card, project, guest and host connection boundaries.
The host service also controls what the Agent and external workers receive.

## Summary

### What Tsugumi takes on

A Tsugumi project is code to run and the definition of the environment it runs in. Opening and running a project is the user's decision to trust its contents, no different from running `make` in a repository you `git clone`d or executing a Jupyter notebook you received. Tsugumi's job is to run it as its author wrote it, so Tsugumi neither judges nor guarantees whether the project's author can be trusted. That is the boundary of Tsugumi's guarantees.

Inside it, Tsugumi takes on the following:

- **AI does not widen permissions without the user's approval.** A save in which the Agent gives a Card the network, running without isolation, a Mac folder or an external Nix source does not go through until the user approves (§7). For a tool you hand work to AI with, this is the boundary that matters most.
- **Card isolation.** With network grants off and filesystem isolation on, a Card cannot reach the network and cannot see the project's definitions, the authentication token or other Cards' work areas. The project records any wider access (§4).
- **The permissions written are visible.** Widened permissions are in readable files, `_card.json` and `tsugumi.json`, and appear on the Card's icons on the Canvas and in the Card's settings. The `.tsugumi/` of a project from someone else is inspected before use.
- **The Mac and other projects are protected.** In the app (the shared VM), even if a project could not be trusted, its files and processes reach only that project and the folders explicitly shared with it (§2, §3).

The table below lists the limits of these guarantees. Details are in each section.

| Outside the guarantees | Why | Details |
| --- | --- | --- |
| Opening someone else's project, there is no confirmation for `sandbox` and `grants` written in `_card.json`, or for the Nix sources pinned in `tsugumi.lock` | Opening a project is a decision to trust its contents ("What Tsugumi takes on" above). The permissions written are visible in the files and on the Cards, and only when the Agent adds one does it wait for approval | [§9](#9-known-limitations-and-cautions) |
| With the Agent, the code, logs and input and output values the Agent reads are sent to the model's provider | Having an outside model read something means sending it. Only what the Agent reads is sent, and whether to use it, and which provider, is the user's choice. Without the Agent, every operation can be done from the UI | [What is sent to the model](#what-is-sent-to-the-model) |
| With web search on, search terms go to the search service. It cannot be guaranteed that unpublished data stays out of them | Off by default; turning it on is the user's choice. While off, the model is not shown the search tool | [Web search](#web-search) |
| Isolation between projects is on one Linux kernel and does not prevent kernel vulnerabilities | The same isolation as containers; the Mac is separated by the VM. Do not open at the same time projects you want strongly separated | [§9](#9-known-limitations-and-cautions) |
| Even with `grants.network: false`, Nix's fixed-output derivations can communicate at build time | How Nix works. What is fetched is checked against its hash and discarded if it does not match, so the content cannot change. What goes out is the request to the source the author wrote | [§5](#5-limits-on-nix-evaluation) |

## 1. The trust boundaries

There are four boundaries, from the innermost outwards.

| Boundary | Protects | Mechanism |
| --- | --- | --- |
| Card (worker) ⇄ tsugumid | The whole project, the authentication token, other Cards' work areas and the outside network when isolated | bubblewrap separating namespaces and dropping capabilities |
| project ⇄ project | Other projects' files, processes, run histories and build roots | Own uid, namespaces and cgroup (managed by `tsugumi-vmd`) |
| VM ⇄ macOS | All of the Mac's files and the home folder | `Virtualization.framework`. Only the open projects' folders and folders specified in Card definitions or the chat (§2) are shared |
| The RPC on macOS ⇄ other processes and the network | The authority to operate the core | Listening only on `127.0.0.1`, token authentication, checking the WebSocket's `Origin` |

## 2. VM ⇄ macOS

### What the VM is given

| Device | Contents | Writable |
| --- | --- | --- |
| rootfs | The [release-built EROFS image](VMStoreHealth.md#guest-image) with Linux, Nix, Tsugumi, the worker and project tools | Read-only |
| Store disk | `~/Library/Application Support/Tsugumi/vm/store.img` (the Nix store shared by all projects; for a development build from source, under `Tsugumi-dev/`, and likewise below) | Yes |
| virtiofs (per slot) | The open project's folder (`project`) and its control folder (`control`) | Yes |
| virtiofs (same slot, only when specified) | Mac folders specified in a Card's settings (`host-<tag>`) and Mac folders attached to the chat (`chat-<tag>`) | `host-` only with `write: true`; `chat-` read-only |
| virtiofs `rosetta` (only when Rosetta is installed) | Rosetta for Linux (`VZLinuxRosettaDirectoryShare`), for Cards run as `x86_64-linux`; registered with binfmt_misc by the VM's supervisor (`vm/guest/vm-system.nix`) | Read-only |
| vsock | The control channel (1024) and the RPC relay (1025) | — |
| Network | NAT (`VZNATNetworkDeviceAttachment`) | — |

- **Of the Mac's files, the VM sees only the folder of each open project and its control folder, and the folders explicitly specified in that project's Card definitions or chat.** Neither the whole home folder nor other projects are shared. Stopping a project ends its slot's sharing; closing a UI leaves the resident environment running.
- There are two routes for sharing a Mac folder with the VM (`hostd/src/environment.cc`; the signed helper applies the resulting mount plan).
  - **A Card's "Allow access to folders of the host"** (`grants.hostPaths` in `_card.json`): each Card grants a name declared in its containing `tsugumi.json`. The human binds that name to a folder in host settings before the first run that needs it. In the shared VM, hostd shares only bound folders used by the accepted Card definitions and workers receive only their own grants. A Card with `write: false` receives a read-only mount even if another Card uses the same folder with write access. Agent edits widening a grant require approval (§7, §9).
  - **Attaching a folder to the chat**: a folder for the Agent to read is shared read-only with that project.
- Each project is given its own virtiofs device (`tsugumi-<slot>`) for sharing, so replacing one project's shares does not affect others.
- The only entitlement given to the app is `com.apple.security.virtualization` (`vm/macos/Resources/Tsugumi.entitlements`).
- The signed `tsugumi-vmhost` helper holds the file lock (`flock`) on `vmhost.lock`; hostd requests VM operations through the private helper contract.

### The VM's network

The VM's NAT exists **so that `nix-daemon` can fetch the binary cache from `cache.nixos.org` and download the project sources pinned in `tsugumi.lock`**.
A Card with `grants.network: false` is cut off from the network namespace and cannot communicate outside. Only a Card with "Allow network access" turned on in its settings (`grants.network: true` in `_card.json`) can communicate outside through this NAT (§4).

## 3. project ⇄ project (inside the VM)

The managing process inside the VM, `tsugumi-vmd`, starts each project's `tsugumid` under these isolation conditions (`vm/guest/src/vmd_main.cc`).

- **Own uid**: each project is assigned a uid of its own (from 20001, recorded in `/persist/tsugumi/uids`), and root is given up with `setgroups(0)` → `setgid` → `setuid` before starting.
- **Namespaces**: `CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS` are separated.
- **Visible files**: only the paths below are bind-mounted in the mount namespace; the parent of all slots (`/mnt`), the whole store disk (`/persist`) are all unmounted.
  - `/project` … its own project folder
  - `/control` … its own control folder
  - `/scratch` … its own disk-backed invocation scratch (`/persist/run-scratch/<id>`, owned by the project uid, mode 0700). The core validates scratch ownership and private parents, rejects symbolic links and, on Linux, rejects tmpfs/ramfs (`tsugumid/runtime/src/run_scratch.cc`).
  - its own `/tmp` and `/run/tsugumi` (`tmpfs`)
  - `/run/tsugumi/rpc` … the sockets `tsugumid` listens on (WebSocket, Unix socket). They actually live in `/run/tsugumi-rpc/<id>` (a 0700 folder only that project's uid can enter), and `tsugumi-vmd` relays to them. There are no ports of any project on the VM's loopback (a Card allowed the network shares the VM's network, so a port there would let it reach another project's `tsugumid`).
  - `/run/tsugumi/host/<tag>`, `/run/tsugumi/chat/<tag>` … Mac folders shared with that project (§2, only when specified)
  - `/nix` … an overlay mount (the shared store is readable from every project; writes go only through `nix-daemon`)
- **cgroup**: placed at `/sys/fs/cgroup/tsugumi/<id>`. Stopping a project ends exactly the processes in that cgroup with `cgroup.kill`, without affecting other projects.
- **GC roots**: `/nix/var/nix/gcroots/tsugumi/projects/<id>/` is created by `tsugumi-vmd` as root, and the parent directory is not writable from projects, so another project cannot take it over by creating the directory first (checked in `test_shared_vm.py`).
- **Kernel**: Linux 6.12 LTS (`vm/guest/kernel.nix`) without io_uring, and with unprivileged BPF off by default. `/init` uses sysctls to restrict reading kernel addresses and logs, ptrace (descendants only) and planting links and files in shared sticky folders (Yama is enabled; BPF JIT and userfaultfd are not built in).
- **Terminal**: opening the terminal inside the VM enters the project's namespaces with `setns`, joins the same cgroup and drops to the project's uid before starting `bash`. Stopping the project environment ends its terminals too.

### What projects share

- `nix-daemon` and `/nix/store` are shared. Every path in the store is identified by the hash of its inputs, and builds happen in `nix-daemon`'s sandbox (`sandbox = true`), so one project cannot rewrite another's build results. By Nix's design, however, **which paths exist in the store is visible to every project**. Do not put secrets in the Nix store.

The human-authenticated Host environment service in `tsugumi-hostd` launches native project processes and grants their execution-broker capability before reporting readiness. It authenticates the native project endpoint when reattaching a resident process. Stop and observed process death revoke that capability and remove the Agent's endpoint registration. Attached-folder withdrawal replaces the control file immediately.

Native project processes receive a newly constructed environment, never hostd's environment: `HOME` from the OS account record; `TMPDIR=/tmp`; `LANG=C.UTF-8`; and `PATH` containing that account's `.nix-profile/bin`, `/nix/var/nix/profiles/default/bin`, `/run/current-system/sw/bin`, `/usr/bin`, `/bin`, `/usr/sbin`, and `/sbin`. If a system CA bundle is readable, its path sets `SSL_CERT_FILE` and `NIX_SSL_CERT_FILE` (searched in `/etc/ssl/certs/ca-certificates.crt`, `/etc/ssl/cert.pem`, then `/nix/var/nix/profiles/default/etc/ssl/certs/ca-bundle.crt`). Only the project's newly issued `TSUGUMI_EXECUTION_BROKER` and `TSUGUMI_EXECUTION_BROKER_CAPABILITY_FILE` are added for broker access. Provider credentials, shell startup hooks and arbitrary caller variables are not inherited.

The VM helper is launched separately with the same minimal runtime environment plus the named startup overrides `TSUGUMI_VM_X86` and `TSUGUMI_VM_CMDLINE`. The helper/VM test double additionally uses `TSUGUMI_PY_BINDINGS`, `TSUGUMID`, `TSUGUMI_WORKER`, `TSUGUMI_NIX_DIR`, `HOST_LOG`, `HOST_BUSY` and `HOST_PID` (bindings, runtime locations, request log, failure marker and PID file). These exact names are forwarded only to the helper; native project processes do not inherit them. No prefix or arbitrary environment forwarding is allowed.

Terminals on both platforms use the Host terminal service; hostd owns native PTYs and their input, resize, close and output. The shared VM uses its project environment. Native terminal environments are constructed from the OS account and fixed runtime paths: the same `HOME`, `PATH`, `TMPDIR` and `LANG` values as native project processes, OS account `USER`, `LOGNAME` and `SHELL`, `TERM=xterm-256color`, `COLORTERM=truecolor`, and `TSUGUMI_PROJECT` pointing to the project. These are the only variables the host passes to the OS account's shell, started as a login shell (falling back to `/bin/bash` if the account has no shell); no Web host or hostd environment variables are inherited. The shell's normal system-wide and personal startup files then run as in any terminal, and may change `PATH`, export additional variables or load the person's own credentials. The shell also creates its own state, such as `PWD` and `SHLVL`. Commands the user runs can still change the environment or read files accessible to the account.


Only hostd talks to the signed Mac helper, over the owner-only Unix socket using `protocol/vm_host.fbs` (bounded length-prefixed FlatBuffers). The helper retains Virtualization.framework operations, virtiofs shares, vsock relays and the terminal descriptor bridge; readiness, project identity registration, mount selection, broker authorization and lifecycle decisions belong to hostd. The helper stores no preferences and makes no Agent requests. The VM and project processes remain resident after UI disconnection; closing a page does not revoke an environment's execution authorization.

## 4. Card (worker) ⇄ tsugumid

The contract this section implements is [The sandbox contract](SandboxContract.md).

Linux workers use bubblewrap to apply the Card's requested isolation. With filesystem isolation, file visibility is limited to the realized Nix closure, worker files, the active inputs and enabled grants. A PID namespace confines process inspection and signals to the Card's process tree. An IPC namespace confines System V IPC, and private mounts isolate POSIX shared memory and message queues. Identity uses a mapped uid and gid of 65534 with capabilities dropped. Filesystem and network restrictions also control access to the user's files and sockets. The network grant controls sharing the network namespace. With filesystem isolation disabled, host files and pathname-based Unix sockets may be reachable. A Linux host without bubblewrap runs a Card only when it selects none of the isolation bubblewrap provides (filesystem, PID, IPC, identity) and is granted the network. Otherwise the Card fails with a message that names the setting.

The shared VM hides the project's control folder and state behind mounts even when filesystem isolation is disabled. Worker teardown always ends the worker's process group. The core checks the enabled isolation settings and grants against the worker platform, then applies [worker selection](RemoteWorker.md#worker-selection).

### Direct macOS host

Native macOS applies the Card's PID, IPC and identity settings through Seatbelt.

- PID isolation restricts process inspection and signals to the worker and its children. It denies global process enumeration and process-listing sysctls.
- IPC isolation denies POSIX shared memory and named semaphores, System V IPC and Unix socket binds. The Card already has its inherited worker channels. With the network granted, the only allowed Mach lookup is `com.apple.trustd.agent` for certificate validation, and the only allowed outbound Unix socket connection is to `/private/var/run/mDNSResponder` for system DNS resolution.
- Identity isolation keeps the launch uid and uses the worker's private environment and home, the file allowlist, and restrictions on process inspection, Mach services and host sockets. Ungranted home and application files and services such as `com.apple.SecurityServer` and `com.apple.securityd.xpc` are inaccessible. The file rules also apply with `sandbox.filesystem` off.
- Filesystem isolation is partial. Host file metadata is visible and the entire Nix store and selected system paths are readable. Scratch keeps its original path. Named host path grants on macOS run in the shared Linux VM. Execution is limited to store paths, the worker, scratch and the explicit grants.

The network guarantee is partial. With filesystem or identity isolation on, network access to the local host stays blocked even when granted, with the DNS resolver socket as the sole host Unix socket exception.

The [sandbox contract](SandboxContract.md) defines process directories, the
launch environment and artifact access. The [Card ABI](CardABI.md) defines
invocation values and effects.

### Showing Card views

A Card declares its views in `default.nix`. Each is one immutable file in the Nix store. When a Card opens a declared view, the project core pins the file and binds a random session UUID to the presenting subscription. Other Card processes can send opaque bytes to the window if they know its session. The presenting host sends to a Card path or broadcasts to the window. The core delivers only to live Card processes registered on that session and returns an error when the address has no live recipient. See [display messages](CardABI.md#display-messages) for session reuse and lifetime.

The host loads an `html` view in an isolated frame and carries its data over a transferred `MessagePort`. A `dawn` view is a Wasm module that imports the listed WebGPU and message functions. The Mac app runs it in its app process on wasmtime with native Dawn. The Web UI runs it in the page with WebGPU. [Views](CoreAndSDK.md#views)

- Web UI: `/modules/viewer.html` is put in an iframe. Its response's CSP is `sandbox allow-scripts` (without `allow-same-origin`), so the component runs in an opaque origin and can use neither cookies nor `/api/`. With `default-src 'none'` and `connect-src 'none'` it loads nothing and connects nowhere. With `frame-src 'none'` the Card's inner frame (a `srcdoc` document) cannot navigate itself to any URL either. Its script is embedded in the page; the parent page hands over the declared view's HTML, which the host puts in the inner frame, and then the `MessagePort` for its data. This is the only route that may be embedded (`frame-ancestors 'self'`); every other response forbids embedding.
- macOS app: the trusted host is loaded as HTML in a nonpersistent WKWebView without a file read grant. Card code runs in its opaque inner frame. Content rules block network/file resources, a navigation delegate denies external navigation, and native message handlers reject messages from subframes.
- In both, the view's own document has no network access (`connect-src 'none'`), so a chart view has its library and data inlined, and the data come only through the port.

## 5. Limits on Nix evaluation

A Card's `default.nix` may contain any Nix expression, but evaluation enforces these limits (`tsugumid/runtime/src/nix_eval.cc`, `vm/guest/vm-boot.nix`):

- `restrictEval = true` limits reads to participating Card definitions, generated input files and sources materialized from the participating projects' locks. The core verifies each NAR hash before handing its store path to the evaluator. Named lookups from a Card use its owning project's declarations and lock, independently for every external. Host `NIX_PATH` is ignored and `builtins.nixPath` is empty.
- `enableImportFromDerivation = false` and empty `allowedUris` keep evaluation separate from builds and network fetching. Evaluation rejects fetch primitives and `builtins.getFlake`. Explicit lock updates resolve URLs with `fetchTree`. Fixed-output build derivations can fetch content pinned by hash.
- The generic Card expression tool uses the same restricted evaluator. Building a returned derivation requires an explicit build flag and the Nix sandbox. [Evaluating a Card expression](Nix.md#evaluating-a-card-expression)
- Builds run in `nix-daemon`'s sandbox (`sandbox = true`, `build-users-group = nixbld`), and with `trusted-users = root` only, a project's uid cannot rewrite substituters or Nix settings. Custom [binary cache connections](../BinaryCaches.md) are human application settings outside the project. Their keys are trusted across the shared VM; private-cache credentials stay in `0600` application files and a read-only boot snapshot hidden by project mount namespaces.
- The default binary caches are `https://cache.nixos.org` and `https://tsugumi.cachix.org`, with their signing keys pinned. Additional caches require an explicit application setting or host administrator configuration.
- **Fixed-output derivations (`fetchurl` and the like, whose content is pinned by hash) can communicate outside at build time.** That is how Nix works, and it is separate from a Card's `network` setting (the `default.nix` of a `grants.network: false` Card can still fetch data). What is fetched is discarded unless it matches the hash, but the request to the source itself (including any values in the URL) goes out.

## 6. RPC authentication

### Where it listens

- Inside the VM, `tsugumid` opens no TCP port; it listens for WebSockets on the Unix socket `/run/tsugumi/rpc/s` (`--listen-unix-ws`, `vm/guest/src/vmd_main.cc`). It actually lives in `/run/tsugumi-rpc/<id>`, a 0700 folder only that project's uid can enter (§3).
- To the Mac, it is **relayed through vsock only to the host's `127.0.0.1:<free port>`** (`LoopbackBridge`; on vsock 1025, `tsugumi-vmd` connects to the socket by project ID). It is not exposed to the LAN or outside networks.
- With no ports on the VM's loopback, there is no route to `tsugumid` from another project or from a Card allowed the network.
- The Mac's `tsugumi` command uses this route too (relayed by `tsugumi-vmhost`). In a Web UI started from the repository, and with the `tsugumi` command on Linux, `tsugumid` runs directly on the host and listens with a WebSocket on `127.0.0.1` ([Web UI](../WebUI.md)).
- Unix domain sockets, when used, are created with `umask 0177` (permissions `0600`). The app's standard route to `tsugumid` is the WebSocket, and token authentication is always required.
- The host-side `tsugumi-hostd` has separate Agent (`protocol/agent.fbs`) and Host (`protocol/host.fbs`) listeners: Unix sockets and loopback WebSockets using `tsugumi.agent.v1` and `tsugumi.host.v1`. The app connects through the shared C ABI byte transport; the Web host connects directly with generated TypeScript bindings. The browser retains its HTTP/SSE API and never receives a host credential. Host rejects the external-agent token at Hello. `HelloRequest` authenticates distinct user and external-agent principals; only the user principal can register project endpoints or credentials and resolve or revoke grants. Its `host.ready` file holds the tokens (0600, in a 0700 state directory).

### The authentication token

- At each start, `tsugumid` generates a 32-byte cryptographically random token (`crypto::random_token(32)`) and writes it **only to `server.ready` in the control folder (permissions `0600`, written atomically)**. The control folder itself is protected with `0700`.
- A client presents the token in its first `SessionHello` message. It is compared in constant time (`crypto::equal_secret`), and on a mismatch the connection is closed with `unauthenticated`.
- The token never appears in URLs or logs (the WebSocket URL is just `ws://127.0.0.1:<port>/rpc`), and the app keeps it only in memory.
- Under "Connections" in the settings you can copy the URL to connect to and the file path of `server.ready`, but the token's string itself is never shown on screen.

### Protecting the WebSocket

- The endpoint accepts only `GET /rpc` (anything else is 404) and requires `Sec-WebSocket-Protocol: tsugumi.rpc.v1`.
- A connection with an `Origin` header is checked against the allow list (`--allow-origin`; hostd applies the origins supplied by the client starting the environment) and refused with 403 on a mismatch. Ordinary web browsers always send an `Origin` header, so **a browser that opened a malicious web page cannot connect to Tsugumi on `127.0.0.1`** (preventing DNS rebinding and Cross-Site WebSocket Hijacking). Native clients without an `Origin` header (the app itself, the Agent, test scripts) authenticate with the token.
- The connection is closed at once on receiving an unmasked client frame, a frame with reserved bits set, a frame over the size limit, or a FlatBuffers message with an unknown union type or an invalid length (checked in `test_transport.py` and `test_contract.py`).

### Paging cursors

Authenticated clients page through recorded history using an encoded position. Cursors have no per-start signature or expiry.

## 7. The model's API keys (Agent)

- `tsugumi-hostd` stores API keys itself on every platform, macOS included: `host-core/host.sqlite` (0600) within its owner-only state directory (0700). The Mac app, the Web UI and the `tsugumi` command save, remove and check keys only through the human-authenticated `ProviderSetRequest` / `ProviderStatusRequest`; no client keeps a copy, and no RPC returns a stored key. The macOS Keychain and the `ANTHROPIC_API_KEY` / `OPENAI_API_KEY` environment variables are not used.
- Provider status and credential resolution use the same stored key, so a provider is available exactly when hostd holds its key. Reading a stored model catalog requires no key; an empty refresh retains it.
- Settings and provider interpretation belong to hostd. Revisioned updates reject stale edits, and settings contain no credentials. The Mac app and the Web UI use one `desktop` profile per host with a single revision: settings, ChatGPT accounts and Skill folders are the same in both. The settings screen registers or deletes a key and cannot display a saved key.
- libcurl sends model credentials in the HTTPS header (`x-api-key` / `Authorization: Bearer`) without a cookie engine or disk response cache. Keys and OAuth tokens are excluded from settings, conversation replies, events and logs, and are never passed to the project's `tsugumid`, Linux VM or Card workers. Provider errors are scrubbed before crossing that boundary.
- Skill discovery returns metadata only. After a separate Read grant, hostd reads selected Skill files beneath the registered project or attached-folder root, including files outside the project's document index. Symlinks must resolve within that root and the subsequent bounded read refuses symlink replacement.

### Card secrets

A Card that needs a credential (a database password, a data service's token) declares only the names in its definition: `"secrets": ["database_password"]` in `definitions/<id>/_card.json`. The values never enter the project.

- **One store, bound to the project.** The values are a second kind of entry in the same hostd store as the API keys (`host-core/host.sqlite`, 0600), one value per project id and name (`card-secret/<project id>/<name>`). The project id is the one hostd registers for the project's folder (`.tsugumi/project-id`; hostd refuses an id that is registered to another folder). People set, replace and delete values for the open project under **Settings > Card secrets** of that project in the Mac app and the Web UI, through the human-only `CardSecretSetRequest`; `CardSecretListRequest` returns that project's names that are set and never a value. The Agent has no request or tool that reads or sets them, and hostd never reads them from the environment of the process that started it or `tsugumi`.
- **Delivery.** At each invocation, `tsugumid` asks hostd for the values of the names that Card declares, over the execution broker connection its project capability authenticates (the capability file is hidden from Cards and from the project shell). hostd resolves the names only within the project that capability was issued to; a request cannot name another project, so a project never obtains another project's value, even under the same name. The local worker receives them over a private launch channel and sets each one as an environment variable of the same name in the Card's process only: `os.environ["database_password"]` in Python. A remote worker receives them the same way, once its job has built the Card's environment and only the Card's process remains to start (see [Remote execution](RemoteWorker.md#card-secrets)). Names are `[a-z][a-z0-9_]{0,63}`, so they never collide with `PATH`, `HOME` or the `TSUGUMI_*` variables.
- **What is recorded.** The run history records which names were provided to each invocation, never a value. Values are not written to `tsugumi.json`, `params.nix`, derivations, the Nix store, caches, run scratch or the logs Tsugumi writes. RPC traces show them as redacted. A Card whose declared secret is not set fails before it runs, naming the missing secrets.
- **Who receives a value.** Any Card of the project that declares a name receives the value set for that project; a copied or received project has none until a person sets them for it. A change of a secret value retires the Card's resident workers. On a remote worker the values reach only the job of the invocation that declares them, which belongs to that project's scope on the worker; the execution host's owner can read what that host runs, so register only hosts trusted with the project's credentials as well as its inputs.
- **The Card author's responsibility.** Tsugumi does not redact. What the Card does with a value is up to its author: child processes inherit the environment, and a value the Card prints, logs, writes to an output or sends elsewhere is recorded or sent like any other data.

### The ChatGPT plan (Sign in with ChatGPT)

Instead of an API key, the Agent can use the user's ChatGPT (Plus, Pro) plan. This follows the [procedure OpenAI publishes for open-source tools](https://developers.openai.com/siwc/token-sharing-open-source) (ChatGPT plan usage) (`hostd/src/settings.cc` for OAuth and the callback; `hostd/src/credentials.cc` and `hostd/agent/src/provider.cc` for token refresh and revocation).

- Sign-in happens in the default browser. Tsugumi waits for `/auth/callback` on a free port of `127.0.0.1` and closes it as soon as the result arrives. Each time it uses a new `state`, `nonce` and PKCE (S256), and a response whose `state` does not match is discarded. No client secret and no developer API key are used (the first time, Tsugumi registers itself with `dynamic_agent_client` and uses the issued `client_id` for later sign-ins).
- The ID token's RS256 signature is checked against OpenAI's JWKS (`https://auth.openai.com/.well-known/jwks.json`), and its issuer, audience (the issued `client_id`), expiry and `nonce` are checked. Signing back in to a previous account, it also checks that it is the same account (`sub`). If permission to use the plan (`chatgpt.tokens.use.direct`) was not granted, the Agent does not use the plan and suggests granting it again or using an API key.
- The host ID (`ext_agent_host_id`) is a `urn:uuid:` (UUIDv4) made before the first sign-in, containing nothing that identifies the user.
- hostd stores host IDs, registrations, account metadata and OAuth credentials in `host.sqlite` (0600) under the same owner-only state directory. The app and the Web UI share one account profile per host: signing in from either signs in both.
- `SignInBeginRequest` returns an attempt ID and a browser URL. An explicit restart from macOS can replace a pending attempt. Both clients read `SignInStatusRequest` when the Host reports a change and follow their sign-in by attempt ID. The result identifies the registration and plan permission from that attempt. The current accounts are returned alongside it. When first-start setup selects the provider, the Host verifies that the expected registration is still active and usable. `SignInActionRequest` cancels, selects an account, records the welcome message or signs out. PKCE, state, nonce, code exchange, JWKS verification and the loopback callback all run in hostd. Responses contain account status and exclude tokens and the PKCE verifier. Sign in with a browser on the server's machine so it can reach `127.0.0.1`. The Web sign-in controls require a loopback hostname. Other pages explain where to open sign-in and still offer API keys.
- The Agent service serializes refresh and revocation, persists rotated tokens, and refreshes within five minutes of expiry or after a refused stale token. A refresh failure is reported without deleting credentials. Signing out attempts revocation and deletes the local credential even if the remote revocation cannot be confirmed; the UI reports that outcome. Registration metadata and the host ID remain.
- Access tokens go only in the model request header to the Responses API and model-list endpoint; refresh tokens go only to the issuer's token/revocation endpoints. These operations are absent from model tools and conversation events.
- Requests on the plan are sent with `store: false` (asking that the conversation's state not be stored; how data is handled at the destination follows OpenAI's policies). At a usage limit it stops without retrying, and never silently switches to another way of paying.

### What is sent to the model

When the Agent is used, the following is sent to the connected model provider (Anthropic / OpenAI; the ChatGPT plan is OpenAI too):
- The text of the chat conversation
- Information on Cards and visualisations added to the chat as references
- What the Agent read through its tools (the RPC): Card definitions, source code, run logs, input and output values, and the contents read by hostd with `attached_folder_read` from folders attached to the chat
- When asking the guide's thrush a question, and during a demo's guide: the question or tour message, and what the guide reads through authorized tools. The UI sends project references and selected Skill names; the Agent service reads their contents after authorization. The guide has no tools to save, run or stop, and does not change the project. While not connected to a model, the thrush answers from its script and sends nothing.
- With web search turned on under "API" in the settings: the search terms the Agent writes are passed on by the provider to its search service ([Web search](#web-search)).

**Note that using the Agent sends part of the code, logs, input and output data and the contents of attached folders in the project to an outside model provider.** Before using the Agent on a project containing unpublished data or confidential information, check that it may be sent to the provider.

With the Agent disabled, all Cards using the local service without a network grant, and the host-managed binary cache selected, the project's content and data stay in the local execution environment. Nix can still fetch packages. An external push target receives the closures described in the [binary cache boundary](#binary-cache-boundary). A Card allowed `network: true` can send the data it can access (its inputs, the contents of host folders specified for it and so on) to the outside network.

### Web search

A setting that lets the Agent search the web at its own discretion ("Web search" under "API" in the settings). **Off by default**; while off, the model is not shown the tool, and it is in neither requests nor rules.

- **The provider does the search.** It uses Anthropic's `web_search` and OpenAI's `web_search`; Tsugumi does not connect to a search service, and no new API key is needed. Search terms are handled by the provider's data policies. If search is disabled in the connected account's settings, the request fails (organisation settings in Anthropic's Console; the ChatGPT plan's model and workspace policies).
- **Search terms go outside, with the risk of unpublished data in them.** The model writes the search terms, and Tsugumi does not inspect them. The model's rules tell it not to put unpublished data, results, names, paths or code of files and Cards in search terms, but **this cannot be relied on to prevent it**. What the Agent has read may end up in a search term. For projects with unpublished research, decide whether to turn it on considering whether it may reach not only the provider but also the search service.
- **Searches are kept.** Search terms and the URLs found are kept, like other tools, in the chat's rows and the conversation's record (`tools`, `messages`). Even if a response fails and is discarded, the record of the search stays (the terms have already gone out).
- **Search results are outside text.** They may contain instructions to the Agent. The model's rules tell it not to treat them as instructions, but this is not a guarantee either. That a save widening a Card's permissions does not go through without the user's approval ([The Agent's permission boundary](#the-agents-permission-boundary)) does not change with search.
- **Helpers and the guide inherit the provider's web-search setting too.** Conversation compaction disables search for its summary request.
- **Counts and charges.** With Anthropic, searches are limited to five per turn; OpenAI has no such limit. Search may be charged separately from tokens (check the provider's pricing).
- With Anthropic, the basic version (`web_search_20250305`) is used: the newer version with dynamic filtering runs code on the provider's side and is not covered by zero data retention. The provider's opaque search-result blocks are kept in the conversation database (`.tsugumi/agent.sqlite` for a project, the host's `host.sqlite` for Home) and sent back to the provider in the next request. After the setting is turned off, they are not sent in that form; the search terms and URLs are sent as text.

### The Agent's permission boundary

- The only operations the Agent can perform are the published Agent and project RPC tools; it cannot operate the UI or the Mac's file system directly. To `tsugumid` it is one client equal to the UI, going through the same revision checks and conflict detection.
- Tool calls that change something carry a unique `op_id`, and when a response is cut off the applied state is confirmed with `OperationGet`, so blind retries never apply anything twice.
- Arguments the model wrote are checked by the Agent and `tsugumid`, and a tool call that is not a JSON object, or from a response cut off at the output limit, is not run.
- **A Card's permissions belong to the user.** The host always sets `check_permissions: true` on Agent saves and reloads. When a checked save newly gives a Card any of the following, `tsugumid` does not save it and returns `approval_required`, listing each permission that widens (Cards inside a Card in Card included; `tsugumid/runtime/src/permissions.cc`):
  - the network (`network: true`)
  - relaxing isolation or resource limits (`sandbox`)
  - GUI or host paths (`grants.gui` and `grants.hostPaths`)
  - explicit Nix lock updates requested by the Agent (`nix_path_lock_update`)

  The approval works like this:
  - The Agent service requests per-call `CardPermissions` approval, resolved by the user through Agent RPC. Approval binds the conversation, project, call ID, argument digest and exact permission set, and is consumed once. A different save or a general grant cannot use it; the UI presents the request rather than deciding the permission rule.
  - The approval is sent in a field the Agent service fills (`approved_permissions`); model-supplied approval, origin and restriction fields are removed. The host sets the comparison flag again for every attempt, including prepared saves and approval retries. `origin` only labels edits for client synchronization; it selects no restriction.
  - If not allowed, nothing is saved, and the Agent is told to explain the reason to the user.
  - A direct client chooses whether to request this comparison with `check_permissions`, and lists its approvals in `approved_permissions`. The core applies the same comparison to every checked request, whatever its origin label. The UI's ordinary saves are the person's own edits and leave comparison off; its reload dialog compares first and sends the exact approved widenings. A Card on the Canvas shows the permissions it has as icons.
- **Shell commands need a separate Shell grant.** `project_shell` requires Read, Write, Run and Shell. The host applies the caller's exact Card scope when staging files and checks it again before saving. The core hides the real project, credentials and direct local RPC. It exposes only the launcher tools' Nix closure, since other store paths can contain Card sources. Required isolation must succeed before starting the command. See the [shell workspace and limits](Agent.md#commands-in-the-project-environment).
- **The Agent can write a Card's comments too.** The host prepares Agent saves and records comment authorship (`hostd/src/comments.cc`). The project core validates `_comments.json` against its FlatBuffers schema. Saves that follow comments go through the same permission comparison as above.
- **Project tools use two explicit lists.** hostd reads `requests` (ordinary project tools) and `approvalRequests` (per-call approval tools) in `hostd/agent/Shared/agent.json`. Adding an operation to either catalog class takes one line. Chat and the Claude Code plugin use the same catalog, and model/plugin calls outside both lists are rejected. Every project entry is checked against the RPC schema at startup and in tests; host tools such as `attached_folder_read` are declared separately in `hostTools`, so a stale or duplicate name fails loudly. Internal hostd project RPC is encoded independently of this catalog and is never offered as a tool. View state, connection requests and version-note writes stay outside both lists.
- **Each approval authorizes one act with those arguments.** Deleting run history, clearing the project's build cache, deleting selected run scratch, checking out a version, starting versioning, and keeping a named version need a fresh approval each time. These operations require their own approval in addition to any workspace grants. The Agent can inspect usage and propose a cleanup with its targets and sizes; it cannot decide to delete run data on its own.
  - The Agent service reads the targets through authenticated project RPC: all history pages for the selected finished runs and their count; `ProjectCacheUsageRequest` for the current cache's build bytes and the selected scratch folders' bytes; the inspected project revision and version graph for the working project and selected full commit. Missing measurements are shown explicitly as unknown, never as zero. The history description states that pending remote results are retained. Cache deletion removes build roots and unused builds, not run history or scratch.
  - The existing access request/resolve flow carries the core-generated description, proposed arguments and call ID. These requests use the `CardPermissions` access category but produce no standing grant. The person approves or declines in the Tsugumi page, also for Claude Code. There is no “do not ask again” option for these acts.
  - Approval is durably bound to the conversation, project, call ID and normalized arguments. Before consuming it, the Agent service rereads the targets and rejects changes. History deletion also passes the exact approved run IDs, so a run finishing after that check is excluded. A consumed approval cannot authorize another call; completed calls replay their receipt, and interrupted calls query their operation receipt instead of blindly executing again. The request, answer and execution outcome remain in the conversation. Declining returns an explicit result to the model and changes nothing.

  The following project RPC requests are excluded (all names end in `Request`):

  | Requests | Why they remain outside the Agent catalog |
  | --- | --- |
  | `ViewSave`, `ViewSend`, `ViewClose` | A person's selection, camera and open Card views belong to the presenting UI. |
  | `SessionHello`, `ProjectOpen` | Connection and subscription lifecycle belongs to the client, rather than model tools. |

- **External reloads require permission approval.** `project_reload` compares every Card's permissions, including nested Cards and Nix sources, with the accepted document. With `check_permissions: true`, it returns `approval_required` with every unapproved widening and accepts only a request listing all of those in `approved_permissions`, regardless of origin. The host always selects this check for the Agent, and the UIs select it for external reloads. Rejection leaves the accepted revision and externally edited files unchanged. Agent clients supply approvals only after the user grants them; the model cannot set that field.
- **The Claude Code plugin** ([Using it from Claude Code](../claude-code.md)) is `tsugumi mcp`, a client of `tsugumi-hostd`'s Agent socket.
  - It calls hostd as the external-agent principal (`external_token`), with the same host-owned Read, Write, Run and CardPermissions checks as the chat. Pending access requests are presented on the Web UI's page; a disconnected page does not cancel a core conversation. Attached-folder reads require a user-registered folder and its own Read grant.
  - The external principal cannot answer access requests, revoke grants or change settings; those Agent requests require the user's principal (`tests/python/test_claude_code_plugin.py`). It has no Host connection at all, so no terminals, file listings or settings.
  - `tsugumi_open` is hostd's `ShowRequest`. hostd shows the project on a page or opens the browser itself; the external principal gets the Web UI's origin, never the page key (only the user's principal gets the keyed URL).
  - What this boundary protects is **operations through Tsugumi's tools**. Claude Code also has a shell and file operations, so with the same permissions as the user it can rewrite the project's files (`_card.json` and the like) directly and read what the user can read (such as `host.ready`, which holds the user's token, in hostd's state folder). Stopping that is up to Claude Code's own permission settings, not Tsugumi.
- **Attached files need their own Read grant.** The host opens files without following links and rechecks permission before returning results. Directory listings are capped at 5,000 entries. Copy operations recheck grants and Card scope before committing. See [bounded reads and copies](Agent.md#commands-in-the-project-environment).

Approving a scoped Skill grants the helper access to the approved Card scope
for that turn. A later ordinary turn needs the project Read grant to use the
transcript.

## 8. Where data is kept, and how to delete it

| Data | Where | Deleting and managing |
| --- | --- | --- |
| The project's documents and sources | The project folder | Manage in Finder or the like (Tsugumi never deletes the folder itself) |
| Primary run history and kept artifacts | `.tsugumi/runs.sqlite`, `.tsugumi/artifacts/` | Delete finished runs or their logs, recorded values or artifacts under Run History. Artifacts created without KEEP expire when their run ends. |
| Nix store, build roots | `~/Library/Application Support/Tsugumi/vm/store.img` | "Delete" under Build Cache and "Clean shared store" |
| Control folders (token, origins, attached folders) | `~/Library/Application Support/Tsugumi/environments/<id>/control/` | "Reset all environments" |
| API keys | hostd's owner-only `host-core/host.sqlite` (every platform) | Settings > API registers or deletes a key |
| Card secret values (per project) | The same store as the API keys | The project's Settings > Card secrets sets, replaces or deletes a value |
| Project Agent conversations, turns, archives, events, tool receipts, helper records, usage and in-progress turns | The project's `.tsugumi/agent.sqlite` (0600, excluded from git) | Delete a conversation through Agent RPC (including the conversation list); to remove all conversations, stop hostd and remove this file |
| Home Agent conversations; host project/folder registrations, access requests and grants, provider selections and provisioned OAuth credentials | `host-core/host.sqlite` (0600) under the Mac support folder (`Tsugumi/` or `Tsugumi-dev/`), or under the Web data folder on Linux (`TSUGUMI_HOST_STATE` overrides it) | Delete Home conversations through Agent RPC; revoke access or delete credentials in settings; removing the host state does not delete project conversations |
| ChatGPT account metadata and Skill folder settings | hostd's `host.sqlite`, one `desktop` profile per host shared by the app and the Web UI | Settings > API / Skills |

The host service keeps `host.sqlite` and `execution.sqlite` under `host-core`, with the process lock and log in `host.lock` and `host.log`. There is no separate folder-format marker. Project conversation and run-history databases remain outside this folder.

Launchers use `tsugumi-hostd --ensure`. The first host takes `host.lock` and publishes `host.ready`. Later invocations return that readiness record or the published `host.startup-error` recovery record. If the lock is held and both records are missing, the launcher reports who is using the state folder when the process can be verified. Quit that host service before retrying. On exit, a host skips startup-file cleanup if the state folder has been replaced.

The Mac launcher passes the bundle's display name as `--application-name`. The CLI passes its command name, such as `tsugumi web`. A direct launch defaults to `tsugumi-hostd`. The host and project conversation locks publish the name and hostd PID in `host.lock-owner` and `agent.lock-owner`, respectively. A contender uses this information only while the corresponding lock is held and it can verify a live local process that owns the record. Shared folders can be in use by a process whose identity the local machine cannot verify.

## 9. Known limitations and cautions

Most of these spell out, as practical cautions, what lies outside the boundary in the [summary](#what-tsugumi-takes-on) (whether to trust a project's contents).

- **Run scratch remains until a person deletes it.** Invocation outputs and diagnostic work folders can contain sensitive data and grow across runs. Earlier scratch survives failed acceptance and interrupted runs. Starting a run and reopening the project do not clear earlier scratch. In the project's Build Cache settings, review the past scratch folders and their sizes before choosing **Delete listed scratch…**. The latest run is protected for reruns; active runs and pending remote results block deletion. This action removes only the selected local scratch, without deleting history, kept artifacts or saved last inputs. Run History deletion also removes the selected runs' scratch. These actions unlink files; they do not securely erase the underlying disk. The usage notice suggests review when build caches or scratch grow and never deletes data.
- **Check the permissions written in a project you received before running it.** `sandbox` and `grants` (especially `write: true`) in `_card.json`, and the sources named in `tsugumi.json` and pinned in `tsugumi.lock`, are part of the project you are trusting. The Agent requests approval for permission widening and explicit lock updates (§7). Look at the Cards' icons and the isolation and grants in the Card settings. A Card without filesystem isolation can read every Mac folder shared with that project (§4).
- **Projects share one Linux kernel.** An exploited kernel vulnerability could reach other projects, so do not open at the same time projects you want strongly separated (§3).
- **A Web UI run directly on a Mac host from the repository has weaker isolation.** A Card with filesystem isolation disabled can access files with the user's permissions. A Card with network access can reach devices on the same LAN or VPN. The `tsugumi` command uses the shared VM, so this does not apply to it.
- **Releases are signed ad hoc, without Apple's notarisation.** GitHub Actions builds them from this repository's tags (`.github/workflows/release.yml`), and requested updates are installed after their checksum, code signature and more are checked ("Updates" in [Releasing](../../RELEASING.md)). The correctness of updates rests on trusting the people who can write to this repository and its GitHub Releases.

## Binary cache boundary

The pull connection's public signing key grants its publisher authority over Nix store paths whose contents cannot be verified by their addresses alone. Remote workers require valid signatures for those paths and verify content-addressed paths against their recorded hashes. A cache can see store path hashes queried by builds. If a Card embeds data into its Nix definition, that data can travel in the published derivation or build closure. Invocation inputs, outputs and Card secrets stay on the execution channel.

Hostd stores push credentials in its own secret store. Its settings responses carry only the presence of a credential. On remote submission, it sends a cache settings snapshot and the necessary credentials over mutually authenticated TLS. The worker uses private client configuration or the uploader's environment. The protocol traces redact credentials, and Tsugumi keeps uploader output out of invocation logs. The worker gets the same publishing authority the credentials confer. Removing its registration prevents future delivery. Revoke an external provider's credential there to remove authority already granted to that worker.

The managed cache signs uploads admitted by hostd. A registered worker can upload during its job, so trust in the managed cache includes trust in those workers. Cache objects persist until a person removes them.

## Remote worker boundary

A registered execution host receives the Card's inputs, accesses its derivation closure through the binary cache, and receives the values of its [Card secrets](#card-secrets) for the invocations that declare them. Choose a host trusted with those data. It builds and runs the invocation; it does not evaluate Card definitions. The worker keeps Card secret values only in memory and writes them to no journal, blob, log or store.

All worker connections require TLS 1.3 and mutual Ed25519 authentication. hostd verifies the worker's complete SPKI against the registered pin before sending a request. The worker requires a client certificate and checks its SPKI against the owned `authorized_keys` file at each handshake. CA and hostname trust are replaced by these explicit pins; session resumption, tickets and early data are disabled. Removing a key refuses subsequent handshakes; an authenticated request may finish. Private keys stay with their owner and are absent from Host RPC.

Host-side hostd owns execution registrations and authorization. It derives a project scope from device, inode and birth time and replaces caller-supplied scopes. Copied or replaced directories get different scopes; a filesystem without birth time cannot use remote execution. The project's local broker capability is separate from worker authentication and never reaches the worker. Explicit project stop revokes it; UI disconnection and hostd restart preserve it. Replacing a registration invalidates its capabilities before another worker connection.

The worker serves its dashboard on a second listener of its own, `dashboard_port` on the same address, over HTTPS with a self-signed ECDSA P-256 certificate (`STATE/dashboard-identity.pem`; browsers refuse Ed25519 server certificates). It asks for no client certificate. The startup log prints the certificate's SHA-256, and `Capabilities` carries it over the mutually authenticated link, so a host can pin it: the Mac app's dashboard view accepts exactly that certificate and only navigations within its origin. The Web UI opens the page in a browser tab, where a person compares the fingerprint once. The jobs listener and its mutual authentication are unchanged; an authorized host key gives no dashboard rights, and a dashboard login gives no right to run jobs.

There are three kinds of access:

- **Use:** hosts whose keys are in `authorized_keys` run jobs over the mutual TLS listener.
- **View:** anyone who reaches the dashboard port reads the worker's state without logging in: jobs (without the submitting core's identity), disk use, settings, host public keys with their comments and last connection. It carries no secret. Bind `listen_address` accordingly.
- **Administer:** a person with the administrator password removes jobs, edits `authorized_keys`, saves settings, changes the password and restarts the listener. If `STATE/admin-password` is missing at startup, the worker generates a password with 100 bits of randomness and prints it once on standard output. Anyone who can read that output can log in with the initial password while it is valid. The worker persists only a salted scrypt hash (N = 2^15, r = 8, p = 1, 16-byte salt) in `STATE/admin-password` (0600). It reads the hash for each password check and compares in constant time. An administrator can replace the password on the dashboard using the current password, or locally with `tsugumi-workerd admin-password`. Login and password changes share an in-memory failure count. After five consecutive failures both password checks wait for 1 minute, then 3, 5 and 30 minutes after further failures. A successful password check resets the count.

A login starts a random 256-bit session in the listener's memory, sent as the cookie `__Host-tsugumi` (`Secure`, `HttpOnly`, `SameSite=Strict`, `Path=/`, 12 hours). At most 64 sessions exist. A dashboard password change keeps the session that made it and ends all others. A restart ends them all. Every change is a `POST` with an `application/json` body, which a cross-site form cannot send and the listener grants no CORS for. If supplied, `Origin` must be the page's own and `Sec-Fetch-Site` must be `same-origin`. Administrator changes also need the session's CSRF token, which only `/api/state` returns to that session, in an `X-Tsugumi-CSRF` header. Replies forbid framing (`frame-ancestors 'none'`, `X-Frame-Options: DENY`) and caching.

Saved settings are checked before they are written. Extra pull caches are public substituters with keys the worker trusts for every job, so the administrator chooses what the worker's builds may substitute. Key updates serialize and replace `authorized_keys` atomically, preserving other lines. Stop the listener before editing its files by hand to avoid a concurrent update.

Job removal takes the same supervisor and journal locks as the local CLI and leaves a `Released` tombstone. The dashboard never removes a job recorded as queued, running or cancelling, and a supervisor lock always prevents removal.

See [registration](../RemoteExecution.md#install-and-register) and [remote job storage](RemoteWorker.md#journal-and-scratch-lifetime). Run-history deletion does not delete remote job scratch.
