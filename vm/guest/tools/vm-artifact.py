#!/usr/bin/env python3
"""Assemble or verify the guest artifact set; release and local builds share this tool."""
import argparse
import hashlib
import json
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
PAYLOAD = "tsugumi-vm.tar.gz"


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def verify(directory, identity, revision=None):
    manifest = json.loads((directory / "vm-manifest.json").read_text())
    if manifest["identity"] != identity or manifest["system"] != "aarch64-linux":
        raise ValueError("VM artifact definition mismatch")
    if revision is not None and manifest["revision"] != revision:
        raise ValueError("VM artifact revision mismatch")
    required = {PAYLOAD, "vm-licenses.tar.gz"}
    if not required.issubset(manifest["files"]) or not any(
        name == "vm-sources.tar.gz" or name.startswith("vm-sources.tar.gz.part") for name in manifest["files"]
    ):
        raise ValueError("incomplete VM redistribution artifact")
    for name, checksum in manifest["files"].items():
        if Path(name).name != name or digest(directory / name) != checksum:
            raise ValueError(f"VM artifact checksum mismatch: {name}")
    with tarfile.open(directory / PAYLOAD) as tar:
        if set(tar.getnames()) != {"vmlinux", "rootfs.erofs", "vm-version"}:
            raise ValueError("unexpected VM payload contents")
        if set(manifest["members"]) != {"vmlinux", "rootfs.erofs", "vm-version"}:
            raise ValueError("incomplete VM attachment checksums")
        for name, expected in manifest["members"].items():
            actual = hashlib.sha256()
            with tar.extractfile(name) as stream:
                while chunk := stream.read(1024 * 1024):
                    actual.update(chunk)
            if actual.hexdigest() != expected:
                raise ValueError(f"VM attachment checksum mismatch: {name}")
        if tar.extractfile("vm-version").read().decode().strip() != identity:
            raise ValueError("VM payload identity mismatch")
    return manifest


def build(args):
    destination = args.directory.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.image / PAYLOAD, destination / PAYLOAD)
    with tempfile.TemporaryDirectory() as temporary:
        stage = Path(temporary)
        with tarfile.open(destination / PAYLOAD) as tar:
            tar.extractall(stage, members=[tar.getmember(n) for n in ["vmlinux", "rootfs.erofs", "vm-version"]])
        identity = (stage / "vm-version").read_text().strip()
        inventory = stage / "vm-guest"
        subprocess.run(["python3", str(REPO / "vm/guest/tools/vm-inventory.py"), str(stage), identity, str(inventory)], check=True)
        subprocess.run(["python3", str(REPO / "vm/guest/tools/vm-source-bundle.py"), str(inventory), str(destination / "vm-sources.tar.gz")], check=True)
        with tarfile.open(destination / "vm-licenses.tar.gz", "w:gz") as tar:
            tar.add(inventory, arcname="vm-guest")
        manifest = {"identity": identity, "system": "aarch64-linux", "revision": args.revision,
                    "members": {name: digest(stage / name) for name in ["vmlinux", "rootfs.erofs", "vm-version"]},
                    "files": {path.name: digest(path) for path in sorted(destination.iterdir())
                              if path.name in {PAYLOAD, "vm-licenses.tar.gz", "vm-sources.tar.gz"} or path.name.startswith("vm-sources.tar.gz.part")}}
        (destination / "vm-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    files = [destination / name for name in manifest["files"]] + [destination / "vm-manifest.json"]
    (destination / "SHA256SUMS").write_text("".join(f"{digest(path)}  {path.name}\n" for path in sorted(files)))
    verify(destination, identity, args.revision)
    print(identity)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("build")
    create.add_argument("image", type=Path)
    create.add_argument("directory", type=Path)
    create.add_argument("--revision", required=True)
    check = commands.add_parser("verify")
    check.add_argument("directory", type=Path)
    check.add_argument("identity")
    check.add_argument("--revision")
    args = parser.parse_args()
    if args.command == "build":
        build(args)
    else:
        verify(args.directory, args.identity, args.revision)


if __name__ == "__main__":
    main()
