#!/usr/bin/env python3
"""Test signed HTTP substitution in disposable stores; never write the host store.

Pass a small existing, non-content-addressed store file without references.
This exercises Nix's cache/signature contract, not the VM mount or a provider API.
"""

import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("store_file", type=Path)
    args = parser.parse_args()
    path = str(args.store_file)
    if not args.store_file.is_file() or args.store_file.parent != Path("/nix/store"):
        parser.error("pass an existing store file directly under /nix/store")

    with tempfile.TemporaryDirectory(prefix="tsugumi-cache-probe-") as temporary:
        root = Path(temporary).resolve()
        env = os.environ.copy()
        conf = root / "conf"
        conf.mkdir()
        env["NIX_CONF_DIR"] = str(conf)
        env["NIX_USER_CONF_FILES"] = str(conf / "user.conf")
        nix = ["nix", "--extra-experimental-features", "nix-command",
               "--option", "sandbox", "false", "--option", "build-users-group", ""]

        def run(command):
            return subprocess.run(command, env=env, text=True, capture_output=True, check=True).stdout.strip()

        info = json.loads(run(nix + ["path-info", "--store", "daemon", "--json", path]))
        metadata = info[path] if isinstance(info, dict) else info[0]
        if metadata.get("ca") or metadata.get("references"):
            parser.error("use a non-content-addressed file with no references to test signature rejection")

        producer = "local?root=" + str(root / "producer")
        run(nix + ["copy", "--from", "daemon", "--to", producer, path])
        run(["nix-store", "--generate-binary-cache-key", "tsugumi-test", str(root / "secret"), str(root / "public")])
        run(nix + ["store", "sign", "--store", producer, "--key-file", str(root / "secret"), path])
        run(nix + ["copy", "--store", producer, "--to", "file://" + str(root / "cache"), path])

        class Handler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=str(root / "cache"), **kwargs)

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = "http://127.0.0.1:" + str(server.server_port)
            key = (root / "public").read_text().strip()
            run(nix + ["build", path, "--store", "local?root=" + str(root / "consumer"),
                       "--no-link", "--option", "substituters", url, "--option", "trusted-public-keys", key])
            copied = root / "consumer" / path.lstrip("/")
            if copied.read_bytes() != args.store_file.read_bytes():
                raise RuntimeError("substituted file differs from the source")
            rejected = subprocess.run(nix + [
                "build", path, "--store", "local?root=" + str(root / "reject"), "--no-link",
                "--option", "substituters", url, "--option", "trusted-public-keys",
                "untrusted:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
            ], env=env, text=True, capture_output=True)
            if rejected.returncode == 0 or "not signed by any of the keys" not in rejected.stderr.lower():
                raise RuntimeError("expected a signature rejection: " + rejected.stderr)
            print("Signed HTTP substitution passed; unknown signing key rejected.")
        finally:
            server.shutdown()
            thread.join()
            server.server_close()


if __name__ == "__main__":
    main()
