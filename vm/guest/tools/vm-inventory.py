#!/usr/bin/env python3
"""List an extracted guest's runtime packages and bundle SPDX license texts.

Usage: vm-inventory.py <image directory> <identity> <inventory directory> <revision>
Run from the same staged flake used to build the image, on Linux.
"""
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
STORE_ROOT = os.environ.get("TSUGUMI_BUILD_STORE_ROOT", "")
REPOSITORY = "https://github.com/gen740/Tsugumi"
OWN_FILES = {"group", "hosts", "nix.conf", "nsswitch.conf", "passwd"}


def physical(path):
    """The Apple builder keeps its logical /nix/store under /cache."""
    return Path(STORE_ROOT + str(path)) if str(path).startswith("/nix/store/") else Path(path)


def is_own(name):
    return name in OWN_FILES or name.startswith("tsugumi") or name in {"hosq", "hosq-source"}


def run(*cmd):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True, cwd=REPO).stdout.strip()


def tool(attr, name):
    if executable := shutil.which(name):
        return executable
    paths = run("nix", "--store", "local", "build", "--no-link", "--print-out-paths", "--inputs-from", str(REPO), f"nixpkgs#{attr}").split()
    return next(str(Path(p) / "bin" / name) for p in paths if (Path(p) / "bin" / name).exists())


def runtime_packages(store_paths):
    packages = {}
    for path in store_paths:
        # Nix store names start with a 32-character hash and a dash.
        name, version = re.match(r"(.+?)(?:-(\d.*))?$", Path(path).name[33:]).groups()
        if not is_own(name):
            packages[name, version or ""] = {"name": name, "version": version or ""}
    return [packages[key] for key in sorted(packages)]


def write_notices(vm, version, out, revision, packages, metadata, texts, kernel_version):
    shutil.rmtree(out, ignore_errors=True)
    (out / "licenses" / "Tsugumi").mkdir(parents=True)
    shutil.copyfile(REPO / "LICENSE", out / "licenses" / "Tsugumi" / "LICENSE")
    for package in packages:
        meta = metadata.get(package["name"], {})
        package["licenses"] = meta.get("licenses", [])
        # Output suffixes are not part of a package's version.
        package_version, _, output = package["version"].rpartition("-")
        if output in meta.get("outputs", []):
            package["version"] = package_version
    packages = sorted({(p["name"], p["version"]): p for p in packages}.values(),
                      key=lambda p: (p["name"], p["version"]))
    packages.append({"name": "linux", "version": kernel_version, "licenses": ["GPL-2.0-only"]})
    identifiers = {identifier for p in packages for license in p["licenses"] for identifier in license.split(" WITH ")}
    identifiers.add("MIT")
    for identifier in sorted(identifiers):
        shutil.copyfile(texts / f"{identifier}.txt", out / "licenses" / f"{identifier}.txt")
    image = (vm / "vmlinux").read_bytes()
    start, end = image.find(b"IKCFG_ST"), image.find(b"IKCFG_ED")
    if start < 0 or end <= start:
        sys.exit("guest kernel has no embedded configuration")
    (out / "kernel-config").write_bytes(gzip.decompress(image[start + 8:end]))
    inventory = {"vm_version": version,
                 "nixpkgs": json.loads((REPO / "flake.lock").read_text())["nodes"]["nixpkgs"]["locked"]["rev"],
                 "tsugumi": {"name": "Tsugumi", "version": (REPO / "VERSION").read_text().strip(),
                             "licenses": ["MIT"], "source": REPOSITORY, "revision": revision},
                 "packages": packages}
    (out / "inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
    rows = [f"| {p['name']} | {p['version']} | {' / '.join(p['licenses']) or 'No SPDX license metadata'} |"
            for p in [inventory["tsugumi"], *packages]]
    (out / "README.md").write_text(f"""# Guest image notices

Image `{version}`, nixpkgs `{inventory['nixpkgs']}`.
The table lists runtime packages and SPDX identifiers from nixpkgs metadata;
missing metadata is left unspecified. `licenses/` contains one SPDX text per
identifier and Tsugumi's own MIT notice (including hosq).
Tsugumi's source is [this repository]({REPOSITORY}) at `{revision}`;
unmodified OSS uses upstream sources. `kernel-config` is the embedded kernel configuration.

| Package | Version | License (nixpkgs) |
| --- | --- | --- |
""" + "\n".join(rows) + "\n")


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    vm, version, output, revision = sys.argv[1:]
    vm, out = Path(vm).resolve(), Path(output).resolve()
    out_path = run("nix", "eval", "--raw", f"path:{REPO}#packages.aarch64-linux.vm-boot.outPath")
    if Path(out_path).name != version:
        sys.exit(f"image is {version}, staged source builds {Path(out_path).name}")
    store_paths = run(tool("erofs-utils", "dump.erofs"), "--cat", "--path=/store-paths", str(vm / "rootfs.erofs")).split()
    packages = runtime_packages(store_paths)
    expression = f"""
      import {REPO}/vm/guest/vm-licenses.nix {{
        pkgs = (builtins.getFlake "path:{REPO}").inputs.nixpkgs.legacyPackages.aarch64-linux;
        wanted = builtins.fromJSON ''{json.dumps(sorted({p['name'] for p in packages}))}'';
      }}
    """
    metadata = json.loads(run("nix", "eval", "--impure", "--json", "--expr", expression))
    texts = run("nix", "build", "--no-link", "--print-out-paths", "--inputs-from", str(REPO),
                "nixpkgs#spdx-license-list-data.text")
    kernel_version = run("nix", "eval", "--raw", f"path:{REPO}#packages.aarch64-linux.kernel.version")
    write_notices(vm, version, out, revision, packages, metadata, physical(texts) / "text", kernel_version)
    print(f"{out}: notices for {version}")


if __name__ == "__main__":
    main()
