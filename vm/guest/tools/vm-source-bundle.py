#!/usr/bin/env python3
"""Archive the exact source/derivation closure recorded by vm-inventory.py.

The store export contains derivations, every inputSrc (including local filesets,
flake inputs and nixpkgs patches), and every fixed-output build input. It contains
no ordinary compiler/runtime outputs. Rebuilding needs Nix and a Linux builder,
but no checkout or upstream source download.
"""
import json
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

GITHUB_ASSET_LIMIT = 2 * 2**30
# Leave space for gzip/tar overhead; only sources may be split.
CHUNK_SIZE = 1900 * 2**20


def source_paths(graph):
    paths = set(graph)
    for drv in graph.values():
        paths.update(drv["inputs"].get("srcs", []))
        if any("hash" in output or "method" in output for output in drv["outputs"].values()):
            paths.update(output["path"] for output in drv["outputs"].values())
    return sorted(p if p.startswith("/nix/store/") else f"/nix/store/{p}" for p in paths)


def split_archive(path, limit=GITHUB_ASSET_LIMIT, chunk_size=CHUNK_SIZE):
    if path.stat().st_size < limit:
        return [path]
    chunks = []
    with path.open("rb") as stream:
        while stream.tell() < path.stat().st_size:
            chunk = path.with_name(f"{path.name}.part{len(chunks):03d}")
            with chunk.open("wb") as target:
                remaining = chunk_size
                while remaining and (data := stream.read(min(remaining, 1024 * 1024))):
                    target.write(data)
                    remaining -= len(data)
            chunks.append(chunk)
    path.unlink()
    return chunks


def main():
    inventory_dir, out = map(Path, sys.argv[1:])
    inventory = json.loads((inventory_dir / "inventory.json").read_text())
    graph = json.loads((inventory_dir / "derivations.json").read_text())
    fixed = [p if p.startswith("/nix/store/") else f"/nix/store/{p}" for p, drv in graph.items()
             if any("hash" in output or "method" in output for output in drv["outputs"].values())]
    # Build-time source fetchers are part of the source closure too, even when
    # their packages are absent from the runtime image.
    subprocess.run(["nix-store", "--realise", *fixed], check=True, stdout=subprocess.DEVNULL)
    paths = source_paths(graph)
    # flake archive records the actual staged checkout and all pinned flake inputs.
    paths += inventory["flake_sources"]
    paths = sorted(set(subprocess.check_output(["nix-store", "-qR", *paths], text=True).split()))
    if not set(inventory["source_paths"]).issubset(paths):
        raise RuntimeError("source closure does not cover the inventory")
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        with (root / "sources.nar").open("wb") as stream:
            subprocess.run(["nix-store", "--export", *paths], stdout=stream, check=True)
        (root / "README.md").write_text(f"""# Corresponding source of the Tsugumi guest

Image identity: `{inventory['vm_version']}`
Nixpkgs: `{inventory['nixpkgs']}`

`licenses/` records every runtime output plus the kernel. `source-paths.json`
records the complete build source closure, including Tsugumi's filtered native
source, hosq, flake inputs, fetched patches, nixpkgs recipes/configuration and
all recursive derivations. `sources.nar` is a Nix store export of those paths.
The staged checkout is `{inventory['flake_sources'][0]}`; it includes local
changes when this is a development build. It is the source Nix evaluated,
not an archive of HEAD labeled as a dirty build.

On aarch64-linux with Nix (or a configured builder), extract this archive, then:

    nix-store --import < sources.nar
    nix-store --realise {inventory['image_derivation']}

The result contains `tsugumi-vm.tar.gz` (kernel, rootfs and version). The image derivation, every
source input and fixed-output dependency are included; ordinary build outputs
such as compilers are rebuilt or substituted. To evaluate the flake again,
use the exported checkout and the input paths in `flake-archive.json` with
`nix build --override-input <name> path:<exported-input> <checkout>#vm`.
No original checkout or unrecorded source input is required.

If the release has numbered parts, reassemble before extraction:

    cat vm-sources.tar.gz.part* > vm-sources.tar.gz
    tar -xzf vm-sources.tar.gz

Verify the parts against the release's SHA256SUMS before reassembly. Retain
these sources for as long as the corresponding released binaries are offered.
""")
        (root / "source-paths.json").write_text(json.dumps(paths, indent=2) + "\n")
        with tarfile.open(out, "w:gz") as tar:
            def clean(info):
                info.uid = info.gid = info.mtime = 0
                info.uname = info.gname = ""
                return info
            for item in root.iterdir():
                tar.add(item, arcname=item.name, filter=clean)
            tar.add(inventory_dir, arcname="licenses", filter=clean)
    print(f"{out}: {out.stat().st_size} bytes, {len(paths)} source/derivation paths")
    split_archive(out)


if __name__ == "__main__":
    main()
