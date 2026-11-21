"""tsugumi-cached against the real Nix client.

`nix copy --to` pushes over mutual TLS with a client certificate, exactly as a
push target would; `nix copy --from` and a substituter pull into other stores
and check the server's signature. The paths are input-addressed derivation
outputs, which Nix accepts only with a trusted signature."""

from __future__ import annotations

import http.client
import json
import os
import shutil
import signal
import socket
import ssl
import subprocess
import time
from pathlib import Path

import pytest
from harness import peer_tls as TLS

ROOT = Path(__file__).resolve().parents[3]
BUILD = Path(os.environ.get("TSUGUMI_BUILD_DIR", ROOT / "build")).resolve()
CACHED = Path(os.environ.get("TSUGUMI_CACHED", BUILD / "hostd/cache/tsugumi-cached"))

pytestmark = pytest.mark.skipif(shutil.which("nix") is None, reason="needs nix")

# A dependency and a path that refers to it: the push must keep their order.
PATHS_EXPR = """
let
  sh = args: derivation {
    name = "tsugumi-cache-test-" + args.name;
    system = builtins.currentSystem;
    builder = "/bin/sh";
    args = [ "-c" args.script ];
  };
  dependency = sh { name = "dependency"; script = "echo dependency > $out"; };
in
sh { name = "top"; script = "echo ${dependency} > $out"; }
"""


def nix(*args, env, check=True):
    # A nix.conf `ssl-cert-file` (as on some CI hosts) wins over the
    # environment variable; the option wins over both.
    trust = ["--option", "ssl-cert-file", env["NIX_SSL_CERT_FILE"]] if "NIX_SSL_CERT_FILE" in env else []
    got = subprocess.run(["nix", "--extra-experimental-features", "nix-command", *trust, *args],
                         env=env, text=True, capture_output=True, timeout=600)
    if check and got.returncode != 0:
        raise AssertionError(f"nix {' '.join(args)} failed:\n{got.stderr}")
    return got


@pytest.fixture(scope="module")
def built_paths():
    """The closure, built in the default store (input-addressed outputs)."""
    env = {**os.environ}
    top = nix("build", "--impure", "--no-link", "--print-out-paths", "--expr", PATHS_EXPR,
              env=env).stdout.strip()
    closure = nix("path-info", "--recursive", top, env=env).stdout.split()
    assert len(closure) == 2
    return top, closure


class Server:
    def __init__(self, state: Path, authorized: list[str]):
        self.state = state
        state.mkdir(mode=0o700, exist_ok=True)
        keys = state / "authorized_keys"
        keys.write_text("".join(key + " # test client\n" for key in authorized))
        keys.chmod(0o600)
        self.log = open(state.parent / f"{state.name}.log", "ab")
        self.process = subprocess.Popen([str(CACHED), "--state-dir", str(state), "--port", "0"],
                                        stdout=subprocess.PIPE, stderr=self.log, text=True)
        self.info = {}
        try:
            for line in self.process.stdout:
                key, _, value = line.rstrip("\n").partition(": ")
                self.info[key] = value
                if key == "authorized keys":
                    break
            assert "url" in self.info, f"server did not start: {self.info}"
        except BaseException:
            self.stop()
            raise
        self.url = self.info["url"]
        self.port = int(self.url.rsplit(":", 1)[1])
        self.public_key = self.info["signing public key"]
        self.certificate = Path(self.info["certificate"])

    def stop(self, sig=signal.SIGTERM):
        if self.process.poll() is None:
            self.process.send_signal(sig)
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.process.stdout.close()
        self.log.close()

    def store_url(self, identity: Path | None) -> str:
        if identity is None:
            return self.url
        return f"{self.url}?tls-certificate={identity}&tls-private-key={identity}"

    def context(self, identity: Path | None) -> ssl.SSLContext:
        # Verifies the server as curl does: its certificate and the host name.
        context = ssl.create_default_context(cafile=str(self.certificate))
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        if identity is not None:
            context.load_cert_chain(str(identity))
        return context

    def request(self, identity, method, name, body=None):
        connection = http.client.HTTPSConnection("127.0.0.1", self.port,
                                                 context=self.context(identity), timeout=30)
        try:
            connection.request(method, "/" + name, body=body)
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            connection.close()

    def usage(self):
        got = subprocess.run([str(CACHED), "usage", "--state-dir", str(self.state), "--json"],
                             text=True, capture_output=True, check=True)
        return json.loads(got.stdout)


@pytest.fixture
def scratch(tmp_path):
    # Nix's narinfo disk cache would remember earlier answers across cases.
    env = {**os.environ, "XDG_CACHE_HOME": str(tmp_path / "xdg-cache")}
    return tmp_path, env


@pytest.fixture
def client(tmp_path):
    return TLS.identity(tmp_path / "client")


@pytest.fixture
def server(tmp_path, client):
    running = Server(tmp_path / "cache", [TLS.public_key(client)])
    yield running
    running.stop()


def push(server, identity, paths, env, check=True):
    env = {**env, "NIX_SSL_CERT_FILE": str(server.certificate)}
    return nix("copy", "--to", server.store_url(identity), *paths, env=env, check=check)


def narinfos(server):
    return sorted((server.state / "binary-cache").glob("*.narinfo"))


def test_nix_copy_pushes_and_another_store_pulls_with_the_servers_signature(
        server, client, built_paths, scratch):
    tmp_path, env = scratch
    top, closure = built_paths
    push(server, client, [top], env)

    infos = narinfos(server)
    assert {info.name.split(".")[0] for info in infos} == {Path(p).name[:32] for p in closure}
    key_name = server.public_key.split(":")[0]
    for info in infos:
        sigs = [line for line in info.read_text().splitlines() if line.startswith("Sig: ")]
        assert len(sigs) == 1 and sigs[0].startswith(f"Sig: {key_name}:"), info.read_text()
    # Plain Nix files: the folder is itself a file:// binary cache.
    assert (server.state / "binary-cache/nix-cache-info").read_text().startswith("StoreDir: /nix/store\n")
    assert list((server.state / "binary-cache/nar").glob("*.nar.xz"))

    # Another machine: the pusher's own narinfo disk cache remembers the
    # unsigned narinfo it uploaded (docs/BinaryCacheServer.md).
    pull_env = {**env, "NIX_SSL_CERT_FILE": str(server.certificate),
                "XDG_CACHE_HOME": str(tmp_path / "xdg-puller")}
    destination = f"local?root={tmp_path / 'pulled'}"
    nix("copy", "--from", server.store_url(client), "--to", destination,
        "--option", "trusted-public-keys", server.public_key, "--option", "require-sigs", "true",
        top, env=pull_env)
    assert (tmp_path / "pulled" / top.lstrip("/")).read_text().strip().endswith("-tsugumi-cache-test-dependency")
    verified = nix("store", "verify", "--store", destination, "--recursive", "--sigs-needed", "1",
                   "--option", "trusted-public-keys", server.public_key, top, env=pull_env, check=False)
    assert verified.returncode == 0, verified.stderr

    # A substituter, as a remote worker will use it.
    substituted = tmp_path / "substituted"
    nix("build", "--store", f"local?root={substituted}", "--no-link",
        "--option", "substituters", server.store_url(client),
        "--option", "trusted-public-keys", server.public_key, top, env=pull_env)
    assert (substituted / top.lstrip("/")).is_file()

    # Another key does not vouch for these paths.
    other = nix("key", "generate-secret", "--key-name", "other-cache-1", env=env).stdout
    other_public = subprocess.run(["nix", "--extra-experimental-features", "nix-command", "key",
                                   "convert-secret-to-public"], input=other, text=True,
                                  capture_output=True, check=True, env=env).stdout.strip()
    refused = nix("copy", "--from", server.store_url(client), "--to", f"local?root={tmp_path / 'untrusted'}",
                  "--option", "trusted-public-keys", other_public, "--option", "require-sigs", "true",
                  top, env={**pull_env, "XDG_CACHE_HOME": str(tmp_path / "xdg-other")}, check=False)
    assert refused.returncode != 0
    assert "signature" in refused.stderr, refused.stderr


def test_a_push_without_a_registered_key_is_refused(server, built_paths, scratch, tmp_path):
    _, env = scratch
    top, _ = built_paths
    assert push(server, None, [top], env, check=False).returncode != 0
    stranger = TLS.identity(tmp_path / "stranger")
    assert push(server, stranger, [top], env, check=False).returncode != 0
    assert narinfos(server) == []
    assert list((server.state / "binary-cache/nar").iterdir()) == []
    # Reading needs the same registration: no request reaches the cache.
    for identity in (None, stranger):
        with pytest.raises((ssl.SSLError, ConnectionError, http.client.HTTPException)):
            server.request(identity, "GET", "nix-cache-info")


def test_removing_a_key_revokes_new_connections(server, client):
    assert server.request(client, "GET", "nix-cache-info")[0] == 200
    (server.state / "authorized_keys").write_text("# nobody\n")
    with pytest.raises((ssl.SSLError, ConnectionError, http.client.HTTPException)):
        server.request(client, "GET", "nix-cache-info")


def partial_put(server, identity, name, sent, declared):
    context = server.context(identity)
    raw = socket.create_connection(("127.0.0.1", server.port), timeout=30)
    stream = context.wrap_socket(raw, server_hostname="127.0.0.1")
    stream.sendall(f"PUT /{name} HTTP/1.1\r\nHost: cache\r\nContent-Length: {declared}\r\n\r\n".encode()
                   + b"x" * sent)
    return stream


def wait_until(condition, timeout=20):
    deadline = time.monotonic() + timeout
    while not condition():
        assert time.monotonic() < deadline
        time.sleep(0.05)


NAR = "nar/" + "1" * 52 + ".nar.xz"


def test_an_interrupted_upload_is_never_served(server, client, tmp_path):
    uploads = server.state / "uploads"
    stream = partial_put(server, client, NAR, sent=1000, declared=100_000)
    wait_until(lambda: any(uploads.iterdir()))
    stream.close()
    # The connection ended mid-body: the staged file is dropped, nothing served.
    wait_until(lambda: not any(uploads.iterdir()))
    assert server.request(client, "GET", NAR)[0] == 404
    assert server.request(client, "HEAD", NAR)[0] == 404

    # A server that dies mid-upload leaves the staged bytes for a person to
    # see, and still serves nothing under the name.
    stream = partial_put(server, client, NAR, sent=1000, declared=100_000)
    wait_until(lambda: any(uploads.iterdir()))
    server.stop(signal.SIGKILL)
    stream.close()
    restarted = Server(server.state, [TLS.public_key(client)])
    try:
        assert restarted.request(client, "GET", NAR)[0] == 404
        usage = restarted.usage()
        assert usage["incompleteFiles"] == 1 and usage["incompleteBytes"] >= 1000
        assert usage["nars"] == 0 and usage["paths"] == 0
    finally:
        restarted.stop()


def test_uploads_are_checked_before_they_appear(server, client, built_paths, scratch):
    _, env = scratch
    top, closure = built_paths
    dependency = next(path for path in closure if path != top)
    # A NAR whose content is not the hash in its name.
    status, _ = server.request(client, "PUT", NAR, body=b"not that nar")
    assert status == 400
    # One real path to forge narinfos from.
    push(server, client, [dependency], env)
    original = narinfos(server)[0].read_text()
    fake = "0" * 32
    # A narinfo under another path's name.
    assert server.request(client, "PUT", f"{fake}.narinfo", body=original.encode())[0] == 400
    forged = original.replace(Path(dependency).name[:32], fake, 1)

    def changed(field, value):
        line = next(line for line in forged.splitlines() if line.startswith(field + ": "))
        return forged.replace(line, f"{field}: {value}").encode()

    assert server.request(client, "PUT", f"{fake}.narinfo",
                          body=changed("NarHash", "sha256:" + "0" * 52))[0] == 400
    assert server.request(client, "PUT", f"{fake}.narinfo",
                          body=changed("URL", "nar/" + "2" * 52 + ".nar.xz"))[0] == 409
    assert server.request(client, "PUT", f"{fake}.narinfo",
                          body=changed("References", "3" * 32 + "-absent"))[0] == 409
    assert server.request(client, "GET", f"{fake}.narinfo")[0] == 404
    # Names outside the binary-cache layout are not files of the cache.
    assert server.request(client, "PUT", "../escape", body=b"x")[0] in (400, 404)
    assert server.request(client, "PUT", "nix-cache-info", body=b"StoreDir: /x\n")[0] == 403


def test_usage_reports_what_the_folder_holds(server, client, built_paths, scratch):
    _, env = scratch
    top, _ = built_paths
    empty = server.usage()
    assert empty["paths"] == 0 and empty["nars"] == 0 and empty["incompleteFiles"] == 0
    push(server, client, [top], env)
    usage = server.usage()
    files = [p for p in (server.state / "binary-cache").rglob("*") if p.is_file()]
    assert usage["bytes"] == sum(p.stat().st_size for p in files)
    assert usage["paths"] == 2 and usage["nars"] == 2 and usage["bytes"] > empty["bytes"]
