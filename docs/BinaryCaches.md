# Binary caches

A binary cache supplies already-built Nix store paths. Tsugumi verifies their signatures using the public keys you register. This is useful for sharing package builds through Attic, Cachix or another Nix binary cache.

## Mac shared execution environment

Open **Settings → Binary Caches**, add a connection, and enter its name, binary-cache endpoint and signing public key. Save it. The setting applies to every project on this Mac when the shared execution environment next starts. Saving does not stop running workflows. The default `cache.nixos.org` and `tsugumi.cachix.org` caches remain enabled.

For Attic, `attic cache info <cache>` reports the **Binary Cache Endpoint** and **Public Key**; the endpoint includes the cache name. Use those values, rather than the Attic server's API endpoint. See the [Attic user guide](https://docs.attic.rs/user-guide/index.html).

For Cachix, use the cache's `https://<name>.cachix.org` endpoint and its signing public key. The cache administrator or Cachix's setup instructions provide the key. See [Cachix getting started](https://docs.cachix.org/getting-started).

For a private cache, expand **Authentication** and enter the username and password/token used for HTTP authentication by Nix's netrc configuration. Both values are required, and authenticated endpoints must use HTTPS. Caches on the same hostname must use the same credentials because netrc matches hosts rather than URL paths. Public caches need neither field.

These settings grant trust to a cache's signing key across the shared VM. They are human application settings, not fields in `tsugumi.json`, a Card's Nix expression or an Agent tool. Tsugumi consumes packages from configured caches; it does not automatically upload project outputs. Use Attic or Cachix's own publishing workflow when intentionally sharing builds.

### Storage and application

The application support folder contains `binary-caches.json` with mode `0600`. It is outside projects, versions and run history. The VM launcher validates the settings and creates a separate snapshot for the next boot. The guest mounts that snapshot read-only and reads it before starting `nix-daemon`; root is still the only trusted Nix user. Project mount namespaces hide the snapshot, including its netrc file. Removing a connection and restarting the environment removes its credentials from the next snapshot.

Edits do not change the currently running VM's snapshot. Invalid configuration stops preparation with an error instead of silently disabling signature checks or falling back to an unauthenticated endpoint. The cache can miss a package: normal Nix building and the remaining configured caches still apply.

## Web and Linux hosts

A Web client using the Mac shared VM uses the VM's cache settings above. A Linux or other native Web host uses that host's Nix configuration. Configure `substituters` and `trusted-public-keys` through the host's administrator, or use the service's `attic use` / `cachix use` setup as appropriate. Tsugumi does not change a host's system Nix configuration from an untrusted project. See [Nix's custom binary cache configuration](https://nix.dev/guides/recipes/add-binary-cache.html).

## Verification

`macOS/tools/test-binary-cache.py /nix/store/<existing-file>` exercises HTTP substitution and unknown-key rejection in temporary, isolated Nix stores. Choose a small existing store file with no references and no content-addressed metadata. The script reads the host store but never signs, builds in or changes it; all generated keys and stores are removed afterward. This checks the Nix cache contract, not provider authentication or VM boot.
