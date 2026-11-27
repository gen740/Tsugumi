# Tsugumi's own binary cache server

`tsugumi-cached` is a standalone Nix binary cache server. Start it explicitly and configure Nix clients with the endpoint and public signing key. See [binary cache settings](../BinaryCaches.md) for configuring substituters.

The cache stores paths fixed by a Nix definition, such as sources, derivations and their build outputs. [Run history](../RunHistory.md) stores Card inputs and outputs.

## What it speaks

The standard Nix binary-cache protocol over HTTPS, so `nix copy`, substituters and other Nix clients use it unchanged.

| Request | Meaning |
| --- | --- |
| `GET`/`HEAD /nix-cache-info` | `StoreDir`, `WantMassQuery: 1`, `Priority: 30` |
| `GET`/`HEAD /<hash>.narinfo` | A store path's metadata, signed by the server |
| `GET`/`HEAD /nar/<file hash>.nar[.xz\|.zst\|.bz2\|.br\|.lz4\|.lzip]` | A compressed NAR |
| `GET`/`HEAD /<hash>.ls` | A NAR listing, when the uploader wrote one |
| `PUT` of the same names | What `nix copy --to https://...` sends, NAR first and narinfo last |

`Content-Length` and chunked bodies, `Expect: 100-continue`, keep-alive and a `Content-Encoding` on narinfo and listing uploads (Nix's `narinfo-compression`) are understood. Logs, realisations (content-addressed derivations) and any other names are not part of this cache and are refused.

## Storage

The cache is a folder in the standard `file://` layout, so its contents are plain Nix files that `nix copy --from file://...` can read without the server:

```text
STATE/
  binary-cache/      nix-cache-info, <hash>.narinfo, nar/<file hash>.nar.xz
  uploads/           uploads in progress (never served)
  signing-key        the Nix secret key (0600)
  signing-key.pub    the key clients put in trusted-public-keys
  tls-identity.pem   the server's Ed25519 TLS key (0600)
  tls-certificate.pem  the certificate clients trust (public)
  authorized_keys    who may connect
  server.socket      the --socket path, while the server listens on it
```

The state folder defaults to `$XDG_STATE_HOME/tsugumi/cache` or `~/.local/state/tsugumi/cache` (`--state-dir` overrides it). One server owns a folder at a time.

**Writes are atomic.** An upload is written to `uploads/` and reaches its name with one `link(2)`, after it is complete, so a name is served whole or not at all. A NAR must have the hash its name states. A narinfo is accepted only when its NAR is already present with the stated file hash and size, the decompressed NAR is one valid NAR with the stated NAR hash and size, and every reference other than itself is already in the cache under its full store path (hash and name), so a served path unpacks and its closure is always complete. A compressed narinfo or listing upload is limited while it is decompressed. A name that exists keeps its first content; nothing is replaced.

An upload whose connection ends is dropped. If the server itself dies mid-upload, the staged bytes stay in `uploads/` (never under a served name) for a person to remove.

**Nothing is removed automatically**: no garbage collection, no retention period. `tsugumi-cached usage` reports what the folder holds, so a person can decide:

```sh
tsugumi-cached usage --state-dir STATE          # size, store paths, NAR files, unfinished uploads
tsugumi-cached usage --state-dir STATE --json   # {"bytes":…,"paths":…,"nars":…,"incompleteFiles":…,"incompleteBytes":…}
```

To remove paths, stop the server and delete their `.narinfo` files (and NARs no narinfo names) from the folder.

## Signing

The server signs every narinfo it accepts with its own key, replacing any signature the uploader added. Clients never hold the secret key: what they trust is the public key in `signing-key.pub` (also printed at startup and by `tsugumi-cached public-key`, which only reads the keys the server created and changes no file). A new key gets a random name such as `tsugumi-cache-5f0c2a91d3e4`, so the caches of two hosts never share a `trusted-public-keys` name.

A client that pushed a path keeps the unsigned narinfo it uploaded in its own narinfo disk cache (`~/.cache/nix`). It already has that path, so this matters only when testing a push and a pull from one machine: pull with a separate `XDG_CACHE_HOME`.

## Who may connect

Every connection is mutual TLS 1.3, with the same Ed25519 public keys as [Execution Services registrations](../RemoteExecution.md#install-and-register). There is no separate token system. The client presents a certificate whose public key is listed in `authorized_keys` (one `ed25519:` key per line, as in a worker's `authorized_keys`); otherwise the handshake fails before any request is read. Reading needs the same registration as pushing, so a registration is also the right to publish. The server checks what it can before it signs: the content, that it is a NAR, and the closure. For an input-addressed path it cannot check that the content is what the path's derivation builds, so its signature there says that a registered key uploaded it, and every client that trusts the key trusts every registered uploader. The file is re-read at every connection, so removing a line refuses that key's next connection.

A remote worker's existing `identity.pem` holds its key and certificate, so Nix uses it directly through the store URL's `tls-certificate` and `tls-private-key` settings. Nix verifies the server like any HTTPS server: the client trusts `tls-certificate.pem` through the `ssl-cert-file` setting, appended to the system bundle when other HTTPS caches are used too. `NIX_SSL_CERT_FILE` works only where no `nix.conf` sets `ssl-cert-file`. The certificate names `localhost`, `127.0.0.1`, `::1` and each `--name`; it stays byte-identical across restarts while the names and the key do not change.

```sh
tsugumi-cached --state-dir STATE --host 0.0.0.0 --name cache.example.org
# On a client whose key is in STATE/authorized_keys:
store='https://cache.example.org:8766?tls-certificate=ID.pem&tls-private-key=ID.pem'
nix copy --option ssl-cert-file tls-certificate.pem --to "$store" /nix/store/...-example
nix copy --option ssl-cert-file tls-certificate.pem --from "$store" \
  --option trusted-public-keys "$(cat signing-key.pub)" /nix/store/...-example
```

The default listener is `127.0.0.1:8766`; `--host` takes an IP literal and `--port 0` picks a free port (the `url:` line of the startup output reports it). `--socket PATH` adds a Unix listener with exactly the same mutual TLS, for a local relay. A start refuses a path that exists, except a socket this state's server left behind (recorded in `server.socket`) on which nothing listens any more.

## Process boundary

The server links the Nix libraries and runs independently of hostd, the
guest supervisor and each project's core. Operators start it and maintain
its registrations and data directory.
