#!/usr/bin/env python3
"""Writes third-party/vm-boot/ from the boot disk in dist/tsugumi-vm: what the
image really contains (its /store-paths), each package's license as nixpkgs
declares it, its source and patches, the license files of those sources and the
kernel's configuration as embedded in the kernel binary.

    macOS/tools/vm-inventory.py

Run it when the boot disk changes (before macOS/tools/publish-vm.sh): the
release build and publish-vm.sh refuse a boot disk this folder does not describe.
The sources come from cache.nixos.org through the Nix store; nothing is built.
"""
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
VM = REPO / "dist" / "tsugumi-vm"
OUT = REPO / "third-party" / "vm-boot"

# pname in the image -> attribute of nixpkgs whose meta declares its license.
ATTRS = {
    "bash-interactive": "bashInteractive",
    "boehm-gc": "boehmgc",
    "db": "db48",
    "gcc": "gcc-unwrapped",
    "gmp-with-cxx": "gmp",
    "icu4c": "icu",
    "libcap-ng": "libcap_ng",
    "nss-cacert": "cacert",
    "systemd-minimal-libs": "systemd",
    "util-linux-minimal": "util-linux",
    "xgcc": "gcc-unwrapped",
}
# Written in this repository (vm-boot.nix): no source of their own to fetch.
OWN = {"group", "hosts", "nix.conf", "nsswitch.conf", "passwd", "tsugumi-udhcpc", "tsugumi-vm-etc", "tsugumi-vm-init"}
# Their source carries no license file, only a notice pointing at this text.
TEXT_FROM = {"nss-cacert": ("publicsuffix-list", "LICENSE")}  # both MPL-2.0

# Their license is a section of this file.
LICENSE_IN = {"boehm-gc": "README.md"}

LICENSE_FILE = re.compile(
    r"(?i)^(?:(?:doc|docs)/|documentation/licenses/)?"
    r"(?:licen[cs]es?|licensing|copying|copyright|notice|unlicense)[^/]*$"
    r"|^LICENSES/.+$")
NOT_LICENSE = re.compile(r"(?i)\.(py|c|h|cc|cpp|sh|in|am|ac|cmake|rs|html?|pl|m4|yml|yaml|json|tmpl|sgml|xml|3|1|8)$")
ARCHIVE = re.compile(r"\.(tar(\.\w+)?|tgz|zip)$")
SKIP_ARCHIVE = re.compile(r"^sqlite-doc-")  # the documentation, beside sqlite-src


def run(*cmd, **kw):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True, cwd=REPO, **kw).stdout


def tool(attr, name):
    paths = run("nix", "build", "--no-link", "--print-out-paths", "--inputs-from", str(REPO), f"nixpkgs#{attr}").split()
    return next(str(Path(p) / "bin" / name) for p in paths if (Path(p) / "bin" / name).exists())


def base(path):
    return Path(path).name


def is_fixed(drv):
    return any("hash" in o or "method" in o for o in drv["outputs"].values())


def source_urls(drv):
    env = drv["env"]
    urls = env.get("urls") or env.get("url") or ""
    return urls.split()


def group_packages(graph, store_paths):
    """One entry per pname: its paths in the image and its sources."""
    by_output = {o["path"]: name for name, d in graph.items() for o in d["outputs"].values() if "path" in o}
    packages = {}
    for path in store_paths:
        drv = graph[by_output[base(path)]]
        # As builtins.parseDrvName: the version starts at the first "-<digit>".
        name, version = re.match(r"(.+?)(?:-(\d.*))?$", drv["name"]).groups()
        entry = packages.setdefault(name, {"name": name, "version": version or "", "paths": [],
                                           "fixed": set(), "patches": set()})
        entry["paths"].append(base(path))
        entry["fixed"].update(d for d in drv["inputs"]["drvs"] if is_fixed(graph[d]))
        for src in drv["inputs"].get("srcs", []):
            if re.search(r"\.(patch|diff)$", src):
                entry["patches"].add(base(src)[33:])
    return packages


def declared_licenses(rev, names):
    attrs = sorted({ATTRS.get(n, "nix" if n.startswith("nix-") else n) for n in names})
    expr = f"""
      let pkgs = (builtins.getFlake "github:NixOS/nixpkgs/{rev}").legacyPackages.aarch64-linux;
          spdx = l: l.spdxId or l.shortName or l.fullName;
          # A plain license, a list, a compound of several, or one with an exception.
          flat = l:
            if builtins.isString l then [ l ]
            else if builtins.isList l then builtins.concatMap flat l
            else if l ? licenses then flat l.licenses
            else if l ? exception then [ "${{spdx l.license}} WITH ${{spdx l.exception}}" ]
            else [ (spdx l) ];
      in builtins.listToAttrs (map (a: {{ name = a; value = flat (pkgs.${{a}}.meta.license or []); }}) (builtins.fromJSON ''{json.dumps(attrs)}''))
    """
    known = json.loads(run("nix", "eval", "--impure", "--json", "--expr", expr))
    return {n: known[ATTRS.get(n, "nix" if n.startswith("nix-") else n)] for n in names}


def archive_members(archive):
    """(name in the archive, path below its top folder) of each license file."""
    for member in run("/usr/bin/tar", "-tf", archive).splitlines():
        below = member.partition("/")[2]
        if below and not member.endswith("/") and LICENSE_FILE.match(below) and not NOT_LICENSE.search(below):
            yield member, below


def copy_license_files(source, dest):
    """Copies a source's license files; returns how many."""
    count = 0
    if os.path.isdir(source):
        for root, _, files in os.walk(source):
            for f in files:
                below = os.path.relpath(os.path.join(root, f), source)
                if LICENSE_FILE.match(below) and not NOT_LICENSE.search(below):
                    (dest / below).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(os.path.join(root, f), dest / below)
                    count += 1
    elif ARCHIVE.search(source) and not SKIP_ARCHIVE.match(base(source)[33:]):
        members = dict(archive_members(source))
        if members:
            with tempfile.TemporaryDirectory() as tmp:
                subprocess.run(["/usr/bin/tar", "-xf", source, "-C", tmp, *members], check=True)
                for member, below in members.items():
                    if os.path.isfile(os.path.join(tmp, member)):
                        (dest / below).parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(os.path.join(tmp, member), dest / below)
                        count += 1
    return count


def write_readme(inventory):
    rows = []
    for p in inventory["packages"]:
        sources = ", ".join(s["name"] for s in p["sources"] if not re.match(r".*\.(patch|diff)$|^bash\d+-|^readline\d+-", s["name"]))
        rows.append(f"| {p['name']} | {p['version']} | {' / '.join(p['licenses']) or 'Tsugumi (MIT)'} "
                    f"| {sources or 'macOS/Container/vm-boot.nix'} | {len(p['patches'])} |")
    size = sum({s["store_path"]: s["bytes"] for p in inventory["packages"] for s in p["sources"]}.values())
    (OUT / "README.md").write_text(f"""# What the VM's boot disk contains

Generated by `macOS/tools/vm-inventory.py` from the boot disk
`{inventory['vm_version']}` (nixpkgs `{inventory['nixpkgs']}`); do not edit.

- [inventory.json](inventory.json): each package's paths in the image, sources
  (store path, hash, URLs) and patches.
- `licenses/<package>/`: the license files of each package's source, unmodified.
- [kernel-config](kernel-config): the configuration embedded in the distributed
  `vmlinux` (`CONFIG_IKCONFIG`); its origin is in
  [macOS/Container/kernel/SOURCES.md](../../macOS/Container/kernel/SOURCES.md).

The License column is what nixpkgs' `meta.license` declares for the package. It
is a declaration, not a file-by-file audit. The sources listed here add up to
{size / 2**20:.0f} MiB.

**The corresponding source** of this boot disk is
[sources.tar.gz](https://github.com/gen740/tsugumi-vm/releases/download/{inventory['vm_version'].split('-')[0]}/sources.tar.gz)
in the disk's release of gen740/tsugumi-vm, with its recipes and build
instructions. See [LicenseAudit.md](../../docs/LicenseAudit.md).

| Package | Version | License (nixpkgs) | Source | Patches |
| --- | --- | --- | --- | --- |
""" + "\n".join(rows) + "\n")


def main():
    version = (VM / "vm-version").read_text().strip()
    rev = json.loads((REPO / "flake.lock").read_text())["nodes"]["nixpkgs"]["locked"]["rev"]
    drv_path = run("nix", "eval", "--raw", ".#packages.aarch64-linux.vm-boot.drvPath")
    out_path = run("nix", "eval", "--raw", ".#packages.aarch64-linux.vm-boot.outPath")
    if base(out_path) != version:
        sys.exit(f"dist/tsugumi-vm is {version}, this checkout builds {base(out_path)}: run build-image.sh --vm first")
    store_paths = run(tool("erofs-utils", "dump.erofs"), "--cat", "--path=/store-paths", str(VM / "rootfs.erofs")).split()
    graph = json.loads(run("nix", "derivation", "show", "--recursive", drv_path))
    graph = graph.get("derivations", graph)

    packages = group_packages(graph, store_paths)
    # The kernel is copied onto the disk beside the store, so it is not one of its paths.
    kernel = next(d for d in graph.values() if d["name"].startswith("tsugumi-guest-kernel"))
    packages["linux"] = {"name": "linux", "version": kernel["env"]["version"], "paths": [], "patches": set(),
                         "fixed": {d for d in kernel["inputs"]["drvs"] if is_fixed(graph[d])}}

    fixed = sorted({d for p in packages.values() for d in p["fixed"]})
    realised = run("nix-store", "--realise", "--max-jobs", "0", *[f"/nix/store/{d}" for d in fixed],
                   stderr=subprocess.DEVNULL).split()
    outputs = dict(zip(fixed, realised))
    names = [n for n in packages if n not in OWN and n != "linux"]
    licenses = declared_licenses(rev, names)
    licenses["linux"] = ["GPL-2.0-only WITH Linux-syscall-note"]

    shutil.rmtree(OUT, ignore_errors=True)
    (OUT / "licenses").mkdir(parents=True)
    nixpkgs = run("nix", "eval", "--impure", "--raw", "--expr", f'(builtins.getFlake "github:NixOS/nixpkgs/{rev}").outPath')
    shutil.copyfile(Path(nixpkgs) / "COPYING", OUT / "licenses" / "nixpkgs-COPYING")

    inventory = {"vm_version": version, "nixpkgs": rev, "packages": []}
    missing = []
    for name in sorted(packages):
        p = packages[name]
        sources = []
        dest = OUT / "licenses" / name
        copied = 0
        for d in sorted(p["fixed"]):
            path = outputs[d]
            copied += copy_license_files(path, dest)
            if name in LICENSE_IN:
                dest.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(Path(path) / LICENSE_IN[name], dest / LICENSE_IN[name])
                copied += 1
            size = int(run("du", "-sk", path).split()[0]) * 1024
            out = graph[d]["outputs"]["out"]
            sources.append({"name": base(path)[33:], "store_path": base(path), "hash": out.get("hash", ""),
                            "urls": source_urls(graph[d]), "bytes": size})
        if name not in OWN and name not in TEXT_FROM and not copied:
            missing.append(name)
        if name not in OWN and not licenses[name]:
            sys.exit(f"nixpkgs declares no license for {name}: is its attribute in ATTRS?")
        inventory["packages"].append({"name": name, "version": p["version"], "licenses": licenses.get(name, []),
                                      "paths": sorted(p["paths"]), "sources": sources, "patches": sorted(p["patches"])})
    for name, (package, file) in TEXT_FROM.items():
        (OUT / "licenses" / name).mkdir(parents=True, exist_ok=True)
        shutil.copyfile(OUT / "licenses" / package / file, OUT / "licenses" / name / file)
    if missing:
        sys.exit(f"no license file found in the sources of: {', '.join(missing)}")

    image = (VM / "vmlinux").read_bytes()
    start, end = image.find(b"IKCFG_ST"), image.find(b"IKCFG_ED")
    (OUT / "kernel-config").write_bytes(gzip.decompress(image[start + 8:end]))
    (OUT / "inventory.json").write_text(json.dumps(inventory, indent=1, ensure_ascii=False) + "\n")
    write_readme(inventory)
    print(f"{OUT}: {len(inventory['packages'])} packages of {version}")


if __name__ == "__main__":
    main()
