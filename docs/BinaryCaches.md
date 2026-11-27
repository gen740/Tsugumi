# Binary caches

Open **Settings → Binary Caches** in the Mac or Web app. The pull list controls where Nix looks for store paths and which signing keys it trusts. The push target controls where Tsugumi publishes store paths. Changes are saved as you edit. The next remote job receives the current settings. A job already running keeps its snapshot.

## Pull list

Enter a name, binary-cache endpoint and public signing key for each connection. The default `cache.nixos.org` and `tsugumi.cachix.org` caches remain enabled.

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
| Nix copy | Set the store URL, for example `file:///srv/nix-cache`, an HTTPS endpoint or an S3 bucket. Configure its signing key separately on the Nix producer when the store does not sign uploads. Add the corresponding pull endpoint and public key. |

Use the credential fields for access tokens and keys. Push credentials are stored by hostd outside the project and delivered to a registered worker over TLS when its job needs them. Attic and Cachix use their respective clients. They are packaged in the Mac VM and with the remote worker. On a native Linux host, install the corresponding client on the execution environment's PATH for local publishing.

Tsugumi publishes the `.drv` closure independently of the output setting. For a remote build, the worker retrieves it with signature verification, then uses signed substitutes for the build outputs it can find and builds the rest. If a push target cannot supply the derivation through the pull list, a remote build using that target fails. Locally, the build and invocation can proceed through their shared Nix store when publishing fails.

Project Settings sets the default for **Push build outputs**, which starts on. Each Code Card can inherit the project setting or choose on or off. Off stops future output uploads for that Card. Published paths stay in the cache. The cache holds Nix definitions and their build products. Card call inputs and outputs travel on the execution channel and are recorded according to the project's run-history settings.

**Run History → Info** records whether the invocation built its Nix environment or reused it, the substituter when known, and whether output publishing succeeded. Publishing a remote derivation is required for the job. A failed output upload leaves the completed build available to the invocation.

See the [security boundary](design/security.md#binary-cache-boundary) for what a registered worker and signing key can access. Developers changing cache configuration can run the [binary cache checks](../tests/README.md#manual-tools).
