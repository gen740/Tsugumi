#!/usr/bin/env python3
"""Inventory an extracted release-built guest and its full build source closure.

Usage: vm-inventory.py <image directory> <identity> <inventory directory>
Run from the same staged flake used to build the image, on Linux.
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
STORE_ROOT = os.environ.get("TSUGUMI_BUILD_STORE_ROOT", "")


def physical(path):
    """The Apple builder keeps its logical /nix/store under /cache."""
    return Path(STORE_ROOT + str(path)) if str(path).startswith("/nix/store/") else Path(path)


# Written in this repository (vm-boot.nix): no source of their own to fetch.
OWN = {"group", "hosts", "nix.conf", "nsswitch.conf", "passwd", "tsugumi-store-health", "tsugumi-udhcpc", "tsugumi-vm-etc", "tsugumi-vm-init", "tsugumi-supervisor", "tsugumi-x86_64", "tsugumi-vm-system", "tsugumid"}
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
    if executable := shutil.which(name):
        return executable
    paths = run("nix", "--store", "local", "build", "--no-link", "--print-out-paths", "--inputs-from", str(REPO), f"nixpkgs#{attr}").split()
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
                                           "fixed": set(), "local": set(), "patches": set(), "derivations": set(), "versions": set()})
        entry["versions"].add(version or "")
        entry["version"] = ", ".join(sorted(entry["versions"]))
        entry["paths"].append(base(path))
        entry["derivations"].add(by_output[base(path)])
        entry["fixed"].update(d for d in drv["inputs"]["drvs"] if is_fixed(graph[d]))
        entry["local"].update(drv["inputs"].get("srcs", []))
        for src in drv["inputs"].get("srcs", []):
            if re.search(r"\.(patch|diff)$", src):
                entry["patches"].add(base(src)[33:])
    return packages


def declared_licenses(graph, packages):
    wanted = [{"name": graph[path]["name"], "path": base(path)}
              for package in packages.values() if package["name"] not in OWN and package["name"] != "linux"
              for path in package["derivations"]]
    expression = f"""
      import {REPO}/macOS/Container/vm-licenses.nix {{
        pkgs = (builtins.getFlake "path:{REPO}").inputs.nixpkgs.legacyPackages.aarch64-linux;
        wanted = builtins.fromJSON ''{json.dumps(wanted)}'';
      }}
    """
    known = json.loads(run("nix", "eval", "--impure", "--json", "--expr", expression))
    missing = [item for item in wanted if not known.get(item["path"])]
    if missing:
        sys.exit(f"no license metadata for actual output owners: {missing}")
    return {package["name"]: sorted({license for path in package["derivations"] for license in known[base(path)]})
            for package in packages.values() if package["name"] not in OWN and package["name"] != "linux"}


def archive_members(archive):
    """(name in the archive, path below its top folder) of each license file."""
    for member in run("/usr/bin/tar", "-tf", archive).splitlines():
        below = member.partition("/")[2]
        if below and not member.endswith("/") and LICENSE_FILE.match(below) and not NOT_LICENSE.search(below):
            yield member, below


def copy_license_files(source, dest):
    """Copies a source's license files; returns how many."""
    source = str(physical(source))
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


def write_readme(inventory, out):
    rows = []
    for p in inventory["packages"]:
        sources = ", ".join(s["name"] for s in p["sources"] if not re.match(r".*\.(patch|diff)$|^bash\d+-|^readline\d+-", s["name"]))
        rows.append(f"| {p['name']} | {p['version']} | {' / '.join(p['licenses']) or 'Tsugumi (MIT)'} "
                    f"| {sources or 'macOS/Container/vm-boot.nix'} | {len(p['patches'])} |")
    size = sum({s["store_path"]: s["bytes"] for p in inventory["packages"] for s in p["sources"]}.values())
    (out / "README.md").write_text(f"""# What the release-built guest contains

Generated by `macOS/tools/vm-inventory.py` of
[Tsugumi](https://github.com/gen740/Tsugumi) from the boot disk
`{inventory['vm_version']}` (nixpkgs `{inventory['nixpkgs']}`); do not edit.

- `inventory.json`: each package's paths in the image, sources
  (store path, hash, URLs) and patches.
- `licenses/<package>/`: the license files of each package's source, unmodified.
- `kernel-config`: the configuration embedded in the distributed `vmlinux`
  (`CONFIG_IKCONFIG`); its origin is in Tsugumi's `macOS/Container/kernel/SOURCES.md`.

The License column is what nixpkgs' `meta.license` declares for the package. It
is a declaration, not a file-by-file audit. The sources listed here add up to
{size / 2**20:.0f} MiB.

**The corresponding source** is `vm-sources.tar.gz` (or its numbered parts)
in the same app release as `tsugumi-vm.tar.gz`, with recipes and rebuild
instructions. Development artifacts carry the same files. See Tsugumi's `docs/LicenseAudit.md`.

| Package | Version | License (nixpkgs) | Source | Patches |
| --- | --- | --- | --- | --- |
""" + "\n".join(rows) + "\n")


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    vm, version, output = sys.argv[1:]
    vm, out = Path(vm).resolve(), Path(output).resolve()
    rev = json.loads((REPO / "flake.lock").read_text())["nodes"]["nixpkgs"]["locked"]["rev"]
    out_path = run("nix", "eval", "--raw", f"path:{REPO}#packages.aarch64-linux.vm-boot.outPath")
    if base(out_path) != version:
        sys.exit(f"image is {version}, staged source builds {base(out_path)}")
    store_paths = run(tool("erofs-utils", "dump.erofs"), "--cat", "--path=/store-paths", str(vm / "rootfs.erofs")).split()
    payload_derivation = run("nix", "eval", "--raw", f"path:{REPO}#packages.aarch64-linux.vm.drvPath")
    graph = json.loads(run("nix", "derivation", "show", "--recursive", payload_derivation))
    graph = graph.get("derivations", graph)

    packages = group_packages(graph, store_paths)
    # The kernel is copied onto the disk beside the store, so it is not one of its paths.
    kernel = next(d for d in graph.values() if d["name"].startswith("tsugumi-guest-kernel"))
    packages["linux"] = {"name": "linux", "version": kernel["env"]["version"], "paths": [], "patches": set(),
                         "fixed": {d for d in kernel["inputs"]["drvs"] if is_fixed(graph[d])},
                         "local": set(kernel["inputs"].get("srcs", [])),
                         "derivations": {name for name, drv in graph.items() if drv is kernel}}

    fixed = sorted({d for p in packages.values() for d in p["fixed"]})
    run("nix-store", "--realise", *[d if d.startswith("/nix/store/") else f"/nix/store/{d}" for d in fixed],
                   stderr=subprocess.DEVNULL).split()
    outputs = {path: str(Path("/nix/store") / graph[path]["outputs"]["out"]["path"]) for path in fixed}
    licenses = declared_licenses(graph, packages)
    licenses["tsugumid"] = ["MIT"]
    licenses["linux"] = ["GPL-2.0-only"]

    shutil.rmtree(out, ignore_errors=True)
    (out / "licenses").mkdir(parents=True)
    nixpkgs = run("nix", "eval", "--impure", "--raw", "--expr", f'(builtins.getFlake "github:NixOS/nixpkgs/{rev}").outPath')
    shutil.copyfile(physical(nixpkgs) / "COPYING", out / "licenses" / "nixpkgs-COPYING")

    archive = json.loads(run("nix", "flake", "archive", "--json", f"path:{REPO}"))
    def flake_paths(node):
        return [node["path"]] + [path for child in node.get("inputs", {}).values() for path in flake_paths(child)]
    source_paths = sorted({str(Path("/nix/store") / src) for drv in graph.values() for src in drv["inputs"].get("srcs", [])})
    inventory = {"vm_version": version, "nixpkgs": rev, "packages": [],
                 "image_derivation": payload_derivation, "source_paths": source_paths,
                 "flake_sources": flake_paths(archive),
                 "output_owners": {base(path): base(owner) for owner, drv in graph.items()
                                   for output in drv["outputs"].values()
                                   if (path := output.get("path")) and base(path) in {base(p) for p in store_paths}}}
    (out / "flake-archive.json").write_text(json.dumps(archive, indent=2) + "\n")
    (out / "derivations.json").write_text(json.dumps(graph, indent=2) + "\n")
    missing = []
    embedded = []
    for name in sorted(packages):
        p = packages[name]
        sources = []
        dest = out / "licenses" / name
        copied = 0
        for path in sorted({outputs[d] for d in p["fixed"]} | {str(Path("/nix/store") / src) for src in p["local"]}):
            copied += copy_license_files(path, dest)
            if name in LICENSE_IN and (physical(path) / LICENSE_IN[name]).is_file():
                dest.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(physical(path) / LICENSE_IN[name], dest / LICENSE_IN[name])
                copied += 1
            size = int(run("du", "-sk", str(physical(path))).split()[0]) * 1024
            fixed_output = next((o for d in p["fixed"] for o in graph[d]["outputs"].values() if o.get("path") == base(path)), {})
            sources.append({"name": base(path)[33:], "store_path": base(path), "hash": fixed_output.get("hash", ""), "nar_hash": run("nix-store", "--query", "--hash", path).strip(),
                            "urls": sorted({url for d in p["fixed"] if outputs[d] == path for url in source_urls(graph[d])}), "bytes": size})
        if name in OWN:
            dest.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO / "LICENSE", dest / "Tsugumi-LICENSE")
            licenses[name] = ["MIT"]
            copied += 1
            if name == "tsugumid":
                hosq = run("nix", "eval", "--impure", "--raw", "--expr",
                           f'(builtins.getFlake "path:{REPO}").inputs.hosq.outPath')
                if not copy_license_files(hosq, dest / "hosq"):
                    sys.exit("hosq license is missing")
                embedded.append({"name": "hosq", "licenses": ["MIT"], "source": hosq,
                                 "license_files": "licenses/tsugumid/hosq"})
                flatbuffers = run("nix", "eval", "--raw",
                                  f"path:{REPO}#packages.aarch64-linux.tsugumid.devEnv.TSUGUMI_FLATBUFFERS_SOURCE")
                if not copy_license_files(flatbuffers, dest / "flatbuffers"):
                    sys.exit("FlatBuffers license is missing")
                embedded.append({"name": "FlatBuffers", "licenses": ["Apache-2.0"], "source": flatbuffers,
                                 "license_files": "licenses/tsugumid/flatbuffers"})
        if name not in OWN and name not in TEXT_FROM and not copied:
            missing.append(name)
        if name not in OWN and not licenses[name]:
            sys.exit(f"no license declared for {name}")
        inventory["packages"].append({"name": name, "version": p["version"], "licenses": licenses.get(name, []),
                                      "paths": sorted(p["paths"]), "sources": sources, "patches": sorted(p["patches"]),
                                      "derivations": sorted(p["derivations"])})
    for name, (package, file) in TEXT_FROM.items():
        if name not in packages:
            continue
        (out / "licenses" / name).mkdir(parents=True, exist_ok=True)
        shutil.copyfile(out / "licenses" / package / file, out / "licenses" / name / file)
    if missing:
        sys.exit(f"no license file found in the sources of: {', '.join(missing)}")

    # Header-only flake input compiled into tsugumid, not a separate runtime path.
    inventory["embedded_components"] = embedded
    image = (vm / "vmlinux").read_bytes()
    start, end = image.find(b"IKCFG_ST"), image.find(b"IKCFG_ED")
    if start < 0 or end <= start:
        sys.exit("guest kernel has no embedded configuration")
    (out / "kernel-config").write_bytes(gzip.decompress(image[start + 8:end]))
    (out / "inventory.json").write_text(json.dumps(inventory, indent=1, ensure_ascii=False) + "\n")
    write_readme(inventory, out)
    print(f"{out}: {len(inventory['packages'])} packages of {version}")


if __name__ == "__main__":
    main()
