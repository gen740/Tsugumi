"""Pass Nix outputs between jobs without granting PRs access to Cachix writes."""

import gzip
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PUBLIC_CACHES = ("https://cache.nixos.org", "https://tsugumi.cachix.org")


def run(*arguments, label=None):
    print("+ " + (label or " ".join(arguments)), file=sys.stderr, flush=True)
    return subprocess.run(arguments, check=True, stdout=subprocess.PIPE, text=True).stdout


def name(value, label):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", value):
        raise ValueError(f"invalid {label}: {value!r}")
    return value


def publish_outputs(outputs):
    with open(os.environ["GITHUB_OUTPUT"], "a") as target:
        for key, value in outputs.items():
            if "\n" in value or "\r" in value:
                raise ValueError(f"invalid output {key}")
            target.write(f"{key}={value}\n")


def build(package, extra_packages):
    records = json.loads(run("nix", "build", "--no-link", "--json",
                             *[f".#{item}^*" for item in [package, *extra_packages]]))
    primary = run("nix", "eval", "--raw", f".#{package}.outPath").strip()
    record = next(item for item in records if primary in item["outputs"].values())
    outputs = {"path": primary, "test-support": record["outputs"].get("testSupport", "")}
    roots = sorted({path for item in records for path in item["outputs"].values()})
    out_link = os.environ.get("NIX_OUT_LINK", "")
    if out_link:
        run("nix", "build", "--out-link", out_link, f".#{package}^out")
    return outputs, roots


def store_paths(value):
    if not isinstance(value, list) or not all(
            isinstance(path, str) and re.fullmatch(r"/nix/store/[0-9a-z]{32}-[^/\s]+", path)
            for path in value):
        raise ValueError("artifact contains invalid Nix store paths")
    return value


def export_paths(artifact, roots):
    remaining = set(run("nix-store", "--query", "--requisites", *roots).splitlines())
    cached = {}
    for cache in PUBLIC_CACHES:
        if not remaining:
            break
        result = subprocess.run(
            ["nix", "path-info", "--json", "--store", cache, *sorted(remaining)],
            capture_output=True, text=True, check=False)
        try:
            info = json.loads(result.stdout)
            available = sorted(path for path in remaining if info.get(path))
        except (ValueError, AttributeError):
            # An unavailable cache only makes the artifact larger.
            available = []
        cached[cache] = available
        remaining.difference_update(available)
    print(f"Reusing {sum(map(len, cached.values()))} public paths; "
          f"archiving {len(remaining)} paths", file=sys.stderr)
    # Export only the paths absent from public caches, without copying their
    # already cached dependencies. gzip is available in the runner's Python.
    command = ["nix-store", "--export", *sorted(remaining)]
    with (gzip.open(artifact / "paths.nar.gz", "wb", compresslevel=1) as target,
          subprocess.Popen(command, stdout=subprocess.PIPE) as process):
        shutil.copyfileobj(process.stdout, target)
        if process.wait():
            raise subprocess.CalledProcessError(process.returncode, command)
    return cached


def import_paths(artifact, manifest):
    cached = manifest["cached"]
    if not isinstance(cached, dict) or set(cached) - set(PUBLIC_CACHES):
        raise ValueError("artifact contains an unknown public cache")
    for cache in PUBLIC_CACHES:
        paths = store_paths(cached.get(cache, []))
        if paths:
            # Public paths retain normal Nix signature verification.
            run("nix", "copy", "--from", cache, *paths,
                label=f"Fetch {len(paths)} paths from {cache}")
    # download-artifact selects this workflow run only. The unsigned export
    # goes into this runner's store, never a shared cache or trust configuration.
    command = ["nix-store", "--import"]
    with (gzip.open(artifact / "paths.nar.gz", "rb") as source,
          subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL) as process):
        try:
            shutil.copyfileobj(source, process.stdin)
        except BrokenPipeError:
            pass
        process.stdin.close()
        if process.wait():
            raise subprocess.CalledProcessError(process.returncode, command)
    run("nix-store", "--query", "--requisites", *store_paths(manifest["roots"]))


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("build", "fetch"):
        raise ValueError("usage: nix-cache.py build|fetch")
    package = name(os.environ["NIX_PACKAGE"], "package")
    system = name(os.environ["NIX_SYSTEM"], "system")
    artifact_name = name(os.environ["NIX_ARTIFACT_NAME"], "artifact name")
    actual_system = run("nix", "eval", "--impure", "--raw", "--expr", "builtins.currentSystem").strip()
    if system != actual_system:
        raise ValueError(f"expected {system}, running on {actual_system}")
    fallback = os.environ.get("NIX_ARTIFACT_FALLBACK", "false") == "true"
    artifact = Path(os.environ["RUNNER_TEMP"]) / f"nix-{artifact_name}"

    if sys.argv[1] == "fetch" and fallback:
        manifest = json.loads((artifact / "manifest.json").read_text())
        if manifest["package"] != package or manifest["system"] != system:
            raise ValueError("artifact does not match the requested package and platform")
        if not store_paths(manifest["roots"]):
            raise ValueError("artifact contains no Nix store roots")
        import_paths(artifact, manifest)

    extra_packages = [name(item, "extra package") for item in os.environ.get("NIX_EXTRA_PACKAGES", "").split()]
    outputs, roots = build(package, extra_packages)
    if sys.argv[1] == "build" and fallback:
        artifact.mkdir(parents=True, exist_ok=False)
        cached = export_paths(artifact, roots)
        (artifact / "manifest.json").write_text(json.dumps({
            "package": package, "system": system, "roots": roots, "cached": cached,
        }) + "\n")
        outputs["artifact-path"] = str(artifact)
    publish_outputs(outputs)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(f"Nix job handoff failed: {error}", file=sys.stderr)
        sys.exit(1)
