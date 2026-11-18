#!/usr/bin/env python3
"""Assemble or verify the guest artifact set; release and local builds share this tool."""
import argparse
import hashlib
import json
import shutil
import tarfile
from pathlib import Path

PAYLOAD = "tsugumi-vm.tar.gz"


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def members(payload):
    checksums = {}
    identity = None
    with tarfile.open(payload, mode="r|gz") as tar:
        for entry in tar:
            with tar.extractfile(entry) as stream:
                checksum = hashlib.sha256()
                stamp = bytearray()
                while chunk := stream.read(1024 * 1024):
                    checksum.update(chunk)
                    if entry.name == "vm-version":
                        stamp.extend(chunk)
                checksums[entry.name] = checksum.hexdigest()
                if entry.name == "vm-version":
                    identity = stamp.decode().strip()
    if set(checksums) != {"vmlinux", "rootfs.erofs", "vm-version"}:
        raise ValueError("unexpected VM payload contents")
    return checksums, identity


def verify(directory, identity, revision=None):
    manifest = json.loads((directory / "vm-manifest.json").read_text())
    if manifest["identity"] != identity or manifest["system"] != "aarch64-linux":
        raise ValueError("VM artifact definition mismatch")
    if revision is not None and manifest["revision"] != revision:
        raise ValueError("VM artifact revision mismatch")
    for name, checksum in manifest["files"].items():
        if Path(name).name != name or digest(directory / name) != checksum:
            raise ValueError(f"VM artifact checksum mismatch: {name}")
    checksums, stamp = members(directory / PAYLOAD)
    if checksums != manifest["members"]:
        raise ValueError("VM attachment checksum mismatch")
    if stamp != identity:
        raise ValueError("VM payload identity mismatch")
    return manifest


def build(args):
    destination = args.directory.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    payload = destination / PAYLOAD
    shutil.copyfile(args.image / PAYLOAD, payload)
    checksums, identity = members(payload)
    manifest = {"identity": identity, "system": "aarch64-linux", "revision": args.revision,
                "members": checksums, "files": {PAYLOAD: digest(payload)}}
    manifest_path = destination / "vm-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    hashes = {**manifest["files"], manifest_path.name: digest(manifest_path)}
    (destination / "SHA256SUMS").write_text("".join(f"{hashes[name]}  {name}\n" for name in sorted(hashes)))
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
