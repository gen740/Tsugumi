"""A pinned external flake served over local HTTPS for public RPC scenarios."""

from contextlib import contextmanager
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import ssl
import subprocess
import tarfile
from threading import Thread
from urllib.parse import quote


NIX = ["nix", "--extra-experimental-features", "nix-command flakes"]


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass


@contextmanager
def source_fixture(directory, monkeypatch, *, message="original"):
    root = directory / "external-source"
    root.mkdir()
    certificate, key = root / "cert.pem", root / "key.pem"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-keyout", str(key), "-out", str(certificate), "-days", "1",
                    "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost"],
                   check=True, capture_output=True)
    fixture = root / "fixture"
    fixture.mkdir()
    locked = json.loads((Path(__file__).resolve().parents[3] / "flake.lock").read_text())["nodes"]["nixpkgs"]["locked"]
    pin = "github:NixOS/nixpkgs/" + locked["rev"] + "?narHash=" + quote(locked["narHash"], safe="")
    (fixture / "flake.nix").write_text('''{
  inputs.nixpkgs.url = "''' + pin + '''";
  outputs = { self, nixpkgs }: {
    packages = builtins.mapAttrs (system: pkgs: {
      sample = pkgs.callPackage ({ message ? ''' + json.dumps(message) + ''', src ? null, patches ? [] }:
        pkgs.runCommand "source-fixture" { inherit message src patches; } ''
          mkdir -p "$out"
          printf '%s\\n' "$message" > "$out/value"
          if [ -n "$src" ]; then cat "$src" >> "$out/value"; fi
          for patch in $patches; do cat "$patch" >> "$out/value"; done
        '') { src = null; };
    }) nixpkgs.legacyPackages;
  };
}
''')
    # All network inputs are pinned before the flake is exposed to the project.
    locked_fixture = subprocess.run([*NIX, "flake", "lock", str(fixture)],
                                    capture_output=True, text=True)
    assert locked_fixture.returncode == 0, locked_fixture.stderr
    nar_hash = subprocess.check_output([*NIX, "hash", "path", str(fixture)], text=True).strip()
    with tarfile.open(root / "flake.tar.gz", "w:gz") as archive:
        archive.add(fixture, arcname="fixture")
    (root / "source.txt").write_text("pinned source\n")
    (root / "patch.txt").write_text("pinned patch\n")
    # Keep public roots trusted too: the first real Card evaluation may fetch
    # its pinned nixpkgs even though the fixture itself is already cached.
    public_bundle = os.environ.get("NIX_SSL_CERT_FILE") or os.environ.get("SSL_CERT_FILE") or ssl.get_default_verify_paths().cafile
    bundle = root / "ca-bundle.pem"
    bundle.write_bytes((Path(public_bundle).read_bytes() if public_bundle else b"") + b"\n" + certificate.read_bytes())
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, key)
    server = ThreadingHTTPServer(("127.0.0.1", 0), partial(QuietHandler, directory=str(root)))
    server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    monkeypatch.setenv("NIX_SSL_CERT_FILE", str(bundle))
    address = f"https://localhost:{server.server_port}"
    def fetched(name):
        digest = subprocess.check_output([*NIX, "hash", "file", str(root / name)], text=True).strip()
        return {"fetchurl": {"url": address + "/" + name, "sha256": digest}}
    try:
        yield ({"name": "fixture", "type": "flake", "ref": address + "/flake.tar.gz?narHash=" + quote(nar_hash, safe="")}, fetched)
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
