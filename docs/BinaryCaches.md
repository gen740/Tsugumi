# Binary caches

Open **Settings → Binary Caches** in the Mac or Web app. The pull list controls where Nix looks for store paths and which signing keys it trusts. The push target controls where Tsugumi publishes store paths. Changes are saved as you edit. The next remote job receives the current settings. A job already running keeps its snapshot.

## Pull list

Enter a name, HTTP(S) binary-cache endpoint and public signing key for each connection. The default `cache.nixos.org` and `tsugumi.cachix.org` caches remain enabled.

- Attic reports the binary-cache endpoint and public key with `attic cache info <cache>`. The binary-cache endpoint includes the cache name. [Attic user guide](https://docs.attic.rs/user-guide/index.html)
- Cachix uses `https://<name>.cachix.org`. Get its public key from the cache administrator or the cache's setup instructions. [Cachix getting started](https://docs.cachix.org/getting-started)
- For private HTTP caches, enter the username and password or token under **Authentication**. Authentication requires HTTPS. Nix's netrc configuration matches by hostname, so connections on the same hostname share credentials.

For the Mac's shared VM, the pull settings take effect when the VM next starts. Hostd stores them in its owner-only `binary-caches.json` and creates a read-only guest snapshot. Project mount namespaces hide the snapshot and its netrc file. A native Linux host uses its administrator's Nix substituter configuration for local builds. [Nix binary cache configuration](https://nix.dev/guides/recipes/add-binary-cache.html)

## Push target

| Target | Configuration |
| --- | --- |
| Tsugumi cache (default) | Hostd starts the cache on demand and supplies the signing key to the worker. Remote transfers share the authenticated worker connection. [Tsugumi cache design](design/BinaryCacheServer.md) |
| Attic | Set the server's API endpoint, cache name and push token. Add its binary-cache endpoint and signing key to the pull list. |
| Cachix | Set the cache name and access token. Add the cache's endpoint and public key to the pull list. |
| Nix copy | Set the store URL, for example `file:///srv/nix-cache`, an HTTPS endpoint or an S3 bucket. Configure signing on the Nix producer when the store does not sign uploads. For remote substitution, expose the cache at an HTTP(S) endpoint and add it and its public key to the pull list. |

Use the credential fields for access tokens and keys. Push credentials are stored by hostd outside the project and delivered to a registered worker over TLS when its job needs them. Attic and Cachix use their respective clients. They are packaged in the Mac VM and with the remote worker. On a native Linux host, install the corresponding client on the execution environment's PATH for local publishing.

## What is pushed, and when

Nothing that a pull cache serves is uploaded. Before it copies a path, Tsugumi asks the pull caches (cache.nixos.org, `tsugumi.cachix.org`, the caches in the pull list, Attic) for its narinfo and leaves the path out when one of them has it with a trusted signature, as `cachix push` does for its upstream. It also leaves out what the push target already holds. Glibc, compilers, Python runtimes and fixed-output sources that a pull cache has therefore never travel through Tsugumi: the worker substitutes them from that cache itself.

- **Before a remote job:** the core publishes the part of the Card's `.drv` closure that no pull cache serves: the derivations and the local sources only this host has. This is the only upload that a job waits for. The worker then fetches what it lacks path by path: from a pull cache when one serves the path, otherwise from the push target. It uses signed substitutes for the build outputs it can find and builds the rest. If the push target cannot supply the derivation through the pull list, a remote build using that target fails.
- **After the Card ran:** with **Push build outputs** on, the worker (or, for a local build, the core) uploads the outputs' closure minus what a pull cache or the target already has: in practice, what this machine built itself. The Card starts before any of it is uploaded and its result does not depend on it: an unreachable target or a failed upload is recorded and never fails the Card. The job's result arrives once the upload has finished or failed.

Locally, the build and invocation proceed through the shared Nix store when publishing fails.

Project Settings sets the default for **Push build outputs**, which starts on. Each Code Card can inherit the project setting or choose on or off. Off stops future output uploads for that Card. Published paths stay in the cache. The cache holds Nix definitions and their build products. Card call inputs and outputs travel on the execution channel and are recorded according to the project's run-history settings.

## Run history

**Run History → Info** records whether the invocation built its Nix environment or reused it, the substituter when known, and what output publishing did: how many paths and bytes it uploaded and how many it left out because a cache already had them, or why it failed. Publishing a remote derivation is required for the job. A failed output upload leaves the completed build and the Card's result as they are.

While a remote job runs, its status line names what the worker does, with progress where it moves a closure path by path: fetching (`取得中: heasoft (3/40 · 1.2 GB / 4.0 GB)`) and, after the Card ran, uploading (`キャッシュへ送信中: … (12/34 · …)`). The Agent's run status shows the same activity.

See the [security boundary](design/security.md#binary-cache-boundary) for what a registered worker and signing key can access. Developers changing cache configuration can run the [binary cache checks](../tests/README.md#manual-tools).
