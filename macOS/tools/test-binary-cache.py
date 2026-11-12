#!/usr/bin/env python3
"""Test signed substitution with the VM's cache snapshot, in disposable stores.

Everything happens under one temporary folder: a store path made up for the
test, a signing key, a public HTTP cache and a private HTTPS cache that answers
only HTTP Basic authentication. The host's store and Nix configuration are never
read or written.

Two things are checked:

1. Nix's contract: a path signed by a trusted key is substituted; the same path
   under an unknown key is refused.
2. The snapshot the launcher gives the VM (`nix.conf` and `netrc`, as
   `BinaryCaches.prepare` writes them): Nix downloads from the private cache
   with it, is refused without its netrc, and its `nix.conf` holds no password.

With `--interactive` the snapshot comes from the caller (the Swift launcher
test): this script prints one JSON line with the private cache's `url`,
`publicKey`, `username` and `password`, then reads the snapshot folder's path
from standard input. Without it the script writes the two files itself in the
same format, which checks Nix against that format but not the launcher's code.

This does not exercise a provider's API (Attic, Cachix) or the VM's boot.
"""

import argparse
import base64
import http.server
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading

# Any name with a well-formed hash part: the path is registered, never built.
STORE_PATH = "/nix/store/0123456789abcdfghijklmnpqrsvwxyz-tsugumi-cache-probe"
GUEST_NETRC = "/run/tsugumi/binary-caches/netrc"
USERNAME = "tsugumi-probe"
PASSWORD = "probe-" + base64.b16encode(os.urandom(12)).decode().lower()


def serve(handler, context=None):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    if context:
        server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--interactive", action="store_true",
                        help="take the snapshot folder from the caller (see above)")
    args = parser.parse_args()
    for tool in ("nix", "nix-store", "openssl"):
        if not shutil.which(tool):
            sys.exit(f"{tool} is needed on PATH")

    with tempfile.TemporaryDirectory(prefix="tsugumi-cache-probe-") as temporary:
        root = Path(temporary).resolve()
        conf = root / "conf"
        conf.mkdir()
        env = {k: v for k, v in os.environ.items() if not k.startswith("NIX_") and k != "NETRC"}
        env["NIX_CONF_DIR"] = str(conf)
        env["NIX_USER_CONF_FILES"] = str(conf / "user.conf")
        env["HOME"] = str(root / "home")
        (root / "home").mkdir()
        options = ["--option", "sandbox", "false", "--option", "build-users-group", ""]
        nix = ["nix", "--extra-experimental-features", "nix-command"] + options

        def run(command, **kwargs):
            return subprocess.run(command, env=env, text=True, capture_output=True, check=True, **kwargs).stdout.strip()

        def substitute(store, extra=()):
            """Asks Nix for the path in a fresh store that has only the caches to get it from."""
            return subprocess.run(nix + ["build", STORE_PATH, "--store", "local?root=" + str(root / store), "--no-link",
                                         *extra], env=env, text=True, capture_output=True)

        # A signed path in a cache folder, made without building anything.
        producer = "local?root=" + str(root / "producer")
        made = root / "producer" / STORE_PATH.lstrip("/")
        made.parent.mkdir(parents=True)
        made.write_text("substituted by the Tsugumi cache probe\n")
        run(["nix-store", "--store", producer, *options, "--register-validity"], input=f"{STORE_PATH}\n\n0\n")
        run(["nix-store", "--generate-binary-cache-key", "tsugumi-test", str(root / "secret"), str(root / "public")])
        run(nix + ["store", "sign", "--store", producer, "--key-file", str(root / "secret"), STORE_PATH])
        run(nix + ["copy", "--store", producer, "--to", "file://" + str(root / "cache"), STORE_PATH])
        key = (root / "public").read_text().strip()

        seen = {"authenticated": 0, "refused": 0}
        expected = "Basic " + base64.b64encode(f"{USERNAME}:{PASSWORD}".encode()).decode()

        class Public(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **k):
                super().__init__(*a, directory=str(root / "cache"), **k)

            def log_message(self, *a):
                pass

        class Private(Public):
            def allowed(self):
                if self.headers.get("Authorization") == expected:
                    seen["authenticated"] += 1
                    return True
                seen["refused"] += 1
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="tsugumi-cache-probe"')
                self.send_header("Content-Length", "0")
                self.end_headers()
                return False

            def do_GET(self):
                if self.allowed():
                    super().do_GET()

            def do_HEAD(self):
                if self.allowed():
                    super().do_HEAD()

        # A certificate for localhost that only this test's Nix trusts.
        (root / "tls.cnf").write_text(
            "[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=localhost\n"
            "[ext]\nsubjectAltName=DNS:localhost,IP:127.0.0.1\nbasicConstraints=critical,CA:TRUE\n")
        run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-config", str(root / "tls.cnf"),
             "-keyout", str(root / "tls.key"), "-out", str(root / "tls.crt")])
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(root / "tls.crt"), str(root / "tls.key"))
        env["NIX_SSL_CERT_FILE"] = env["SSL_CERT_FILE"] = str(root / "tls.crt")

        public, public_thread = serve(Public)
        private, private_thread = serve(Private, context)
        try:
            # 1. Nix's contract, over the public cache.
            url = "http://127.0.0.1:" + str(public.server_port)
            got = substitute("consumer", ["--option", "substituters", url, "--option", "trusted-public-keys", key])
            if got.returncode != 0:
                raise RuntimeError("signed substitution failed: " + got.stderr)
            if (root / "consumer" / STORE_PATH.lstrip("/")).read_bytes() != made.read_bytes():
                raise RuntimeError("the substituted file differs from the source")
            rejected = substitute("reject", ["--option", "substituters", url, "--option", "trusted-public-keys",
                                             "untrusted:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="])
            if rejected.returncode == 0 or "not signed by any of the keys" not in rejected.stderr.lower():
                raise RuntimeError("expected a signature rejection: " + rejected.stderr)

            # 2. The VM's snapshot, over the private cache.
            private_url = "https://localhost:" + str(private.server_port)
            if args.interactive:
                print(json.dumps({"url": private_url, "publicKey": key, "username": USERNAME, "password": PASSWORD}),
                      flush=True)
                snapshot = Path(sys.stdin.readline().strip())
            else:
                snapshot = root / "snapshot"
                snapshot.mkdir()
                (snapshot / "nix.conf").write_text(f"extra-substituters = {private_url}\n"
                                                   f"extra-trusted-public-keys = {key}\nnetrc-file = {GUEST_NETRC}\n")
                (snapshot / "netrc").write_text(f"machine localhost login {USERNAME} password {PASSWORD}\n")
            generated = (snapshot / "nix.conf").read_text()
            if PASSWORD in generated or f"netrc-file = {GUEST_NETRC}\n" not in generated:
                raise RuntimeError("the snapshot's nix.conf must name the guest's netrc and hold no password")
            (root / "empty-netrc").write_text("")

            def configure(netrc):
                # As in the VM: fixed base settings, then the snapshot. The
                # guest's netrc path is the one thing this host cannot provide.
                (conf / "nix.conf").write_text(f"substituters =\ntrusted-public-keys =\n!include {snapshot / 'nix.conf'}\n"
                                               f"netrc-file = {netrc}\n")

            configure(root / "empty-netrc")
            refused = substitute("unauthenticated")
            if refused.returncode == 0 or seen["authenticated"] or not seen["refused"]:
                raise RuntimeError("the private cache answered without credentials: " + refused.stderr)
            configure(snapshot / "netrc")
            got = substitute("authenticated")
            if got.returncode != 0 or not seen["authenticated"]:
                raise RuntimeError("substitution with the snapshot's credentials failed: " + got.stderr)
            if (root / "authenticated" / STORE_PATH.lstrip("/")).read_bytes() != made.read_bytes():
                raise RuntimeError("the file from the private cache differs from the source")
            print("Signed substitution passed; unknown signing key rejected; "
                  "private cache used only with the snapshot's credentials.")
        finally:
            for server, thread in ((public, public_thread), (private, private_thread)):
                server.shutdown()
                thread.join()
                server.server_close()


if __name__ == "__main__":
    main()
