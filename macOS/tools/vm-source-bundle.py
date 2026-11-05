#!/usr/bin/env python3
"""Writes the corresponding source of the VM's boot disk as one archive, from
third-party/vm-boot/inventory.json: every package's sources and fetched patches,
nixpkgs at the pinned revision (the recipes and the patches it keeps itself), and
the part of this repository that builds the disk. publish-vm.sh uploads it.

    macOS/tools/vm-source-bundle.py <file.tar.gz>

The source paths come from cache.nixos.org through the Nix store; nothing is built.
"""
import json
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
VM_BOOT = REPO / "third-party" / "vm-boot"
# What `nix build .#packages.aarch64-linux.vm-boot` reads of this repository.
BUILD_FILES = ["flake.nix", "flake.lock", "VERSION", "LICENSE", "THIRD_PARTY_NOTICES.md", "nix", "macOS/Container"]
GITHUB_ASSET_LIMIT = 2 * 2**30


def run(*cmd):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True, cwd=REPO).stdout


def readme(inventory):
    return f"""# Corresponding source of Tsugumi's VM boot disk

Boot disk: `{inventory['vm_version']}`
nixpkgs: `{inventory['nixpkgs']}`

- `sources/`: each package's sources and fetched patches, under their Nix store
  names. `vm-boot/inventory.json` says which belong to which package, with their
  hashes and upstream URLs.
- `nixpkgs/`: nixpkgs at that revision: how each package is configured, built and
  installed, and the patches it keeps in its own tree.
- `tsugumi/`: the part of Tsugumi that builds the disk. The kernel is
  `macOS/Container/kernel.nix` with `macOS/Container/kernel/base-6.12.28.config`.
- `vm-boot/`: the list of the disk's packages, their license files and the kernel's
  configuration as built.

To build the disk (aarch64-linux, or `macOS/Container/build-image.sh --vm` on a Mac):

    cd tsugumi
    nix --extra-experimental-features 'nix-command flakes' build \\
      --override-input nixpkgs path:../nixpkgs "path:.#packages.aarch64-linux.vm-boot"

The result's store name is the boot disk's name above.
"""


def add(tar, path, name):
    def clean(info):
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        return info
    tar.add(path, arcname=name, filter=clean)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1]).resolve()
    inventory = json.loads((VM_BOOT / "inventory.json").read_text())
    top = f"tsugumi-vm-source-{inventory['vm_version'].split('-')[0]}"
    sources = sorted({s["store_path"] for p in inventory["packages"] for s in p["sources"]})
    store = [f"/nix/store/{s}" for s in sources]
    subprocess.run(["nix-store", "--realise", *store], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    nixpkgs = run("nix", "eval", "--impure", "--raw", "--expr",
                  f'(builtins.getFlake "github:NixOS/nixpkgs/{inventory["nixpkgs"]}").outPath')

    with tarfile.open(out, "w:gz", compresslevel=6) as tar, tempfile.TemporaryDirectory() as tmp:
        text = Path(tmp) / "README.md"
        text.write_text(readme(inventory))
        add(tar, text, f"{top}/README.md")
        add(tar, VM_BOOT, f"{top}/vm-boot")
        add(tar, nixpkgs, f"{top}/nixpkgs")
        for path in store:
            add(tar, path, f"{top}/sources/{Path(path).name}")
        archive = Path(tmp) / "tsugumi.tar"
        subprocess.run(["git", "-C", str(REPO), "archive", "--prefix", f"{top}/tsugumi/", "-o", str(archive), "HEAD", *BUILD_FILES], check=True)
        with tarfile.open(archive) as own:
            for member in own.getmembers():
                tar.addfile(member, own.extractfile(member) if member.isfile() else None)

    size = out.stat().st_size
    if size >= GITHUB_ASSET_LIMIT:
        sys.exit(f"{out}: {size} bytes is over what a GitHub release asset may be")
    print(f"{out}: {size / 2**20:.0f} MiB, {len(sources)} sources")


if __name__ == "__main__":
    main()
