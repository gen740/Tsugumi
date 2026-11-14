"""Select CI consumers of changed paths; unknown paths and uncertain diffs run all.

The source filesets in nix/{core,core-tests,client-wasm,web,web-checks}.nix,
the CMake install rules and ci-platform.yml's test commands define this map.
Keep dependencies here, rather than maintaining path filters in each workflow.
"""

import argparse
import json
import subprocess
import sys


SYSTEMS = {
    "x86_64-linux": "ubuntu-24.04",
    "aarch64-linux": "ubuntu-24.04-arm",
    "aarch64-darwin": "macos-26",
}
CORE = {"agent-rpc", "rpc", "host", "remote-execution", "adapters", "sdk", "swift", "vm"}
WEB = {"docs", "web-contracts", "adapters", "swift"}
PLAYER = {"docs", "web-contracts", "swift"}
ALL = CORE | WEB | {"prompt"}
ADAPTER_TESTS = {
    "tests/python/test_claude_code_plugin.py",
    "tests/python/test_web_server.py",
    "tests/python/test_cli.py",
}


def consumers(path):
    if path.startswith(("protocol/", "nix/", ".github/", "tests/ci/")) or path in {
        "flake.nix", "flake.lock", "CMakeLists.txt",
    } or path.endswith(("/CMakeLists.txt", ".cmake")):
        return ALL
    if path in {"README.md", "THIRD_PARTY_NOTICES.md", "tests/README.md"}:
        return {"docs"}
    if path.startswith("docs/"):
        jobs = {"docs"}
        # These are inputs to web.sitePlayer and Swift's DemoThumbnailTests.
        if path.startswith(("docs/site/player/", "docs/site/player-data/assets/")) or path in {
            "docs/CardGuide.md", "docs/assets/tsugumi-icon.svg",
        }:
            jobs |= PLAYER
        # ExplainerTests read the built help's page list and assets.
        if path.startswith("docs/site/"):
            jobs.add("swift")
        # Runnable examples are extracted from the guide, not copied fixtures.
        if path == "docs/CardGuide.md":
            jobs.add("sdk")
        if path == "docs/features.md":
            jobs.add("prompt")
        return jobs
    if path == "macOS/tools/bundle-docs.sh":
        return {"docs", "swift"}
    if path.startswith("macOS/Container/"):
        return {"swift", "vm", "sdk", "host"}
    if path.startswith("macOS/"):
        return {"swift"}
    if path.startswith("web/"):
        return WEB
    if path.startswith("demos/"):
        # ci-core installs demos for Guide RPC; Swift also reads the sources.
        return WEB | {"sdk", "agent-rpc"}
    if path.startswith("ui/"):
        return WEB | {"sdk"}
    if path.startswith("sdk/"):
        # The native runtime installs the SDK; C++ and host tests use it too.
        return CORE | WEB
    if path.startswith("client/"):
        # Shared native client, Swift C interface and the browser Wasm build.
        return CORE | WEB
    if path.startswith(("core/", "workerd/", "tests/core/")):
        return CORE
    if path.startswith("agent/"):
        return ALL
    if path.startswith("cli/"):
        return {"adapters", "swift"}
    if path.startswith("plugins/") or path in ADAPTER_TESTS:
        return {"adapters"}
    if path.startswith("tests/python/"):
        # Includes shared harness files, not just individual SDK test cases.
        return {"sdk", "adapters"}
    if path.startswith("tests/remote/"):
        return {"remote-execution"}
    if path.startswith("tests/prompt/"):
        return {"prompt"}
    if path.startswith("tests/static/"):
        return set()
    # protocol/, flake.{nix,lock}, nix/, CMake, CI/actions/scripts, fixtures,
    # root configuration and every new/unclassified path deliberately run all.
    return ALL


def select(paths=None):
    jobs = set(ALL) if not paths else set().union(*(consumers(path) for path in paths))
    matrix = []
    for system, runner in SYSTEMS.items():
        supported = set(CORE | WEB) - {"docs"}
        if system == "aarch64-darwin":
            supported -= {"web-contracts", "adapters", "sdk", "vm", "remote-execution"}
        else:
            supported.remove("swift")
            if system != "aarch64-linux":
                supported.remove("vm")
            if system != "x86_64-linux":
                supported.remove("remote-execution")
        checks = {job: job in jobs and job in supported for job in sorted((CORE | WEB) - {"docs"})}
        checks["native"] = any(checks[job] for job in CORE)
        checks["web"] = checks["web-contracts"] or checks["adapters"]
        if any(checks.values()):
            matrix.append({"system": system, "runner": runner, "checks": checks})
    return {"docs": "docs" in jobs, "prompt": "prompt" in jobs,
            "platform": bool(matrix), "matrix": {"include": matrix}}


def changed_paths(event, base, head):
    if event not in {"pull_request", "push"} or not base or set(base) == {"0"}:
        return None
    # PRs compare their branch against its merge base, not a moving base tip.
    revision = [f"{base}...{head}"] if event == "pull_request" else [base, head]
    try:
        result = subprocess.run(
            ["git", "diff", "--name-only", "--no-renames", "-z", *revision, "--"],
            check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        return result.stdout.decode("utf-8").rstrip("\0").split("\0") if result.stdout else None
    except (subprocess.CalledProcessError, UnicodeError):
        print("Cannot determine changed paths; selecting all checks.", file=sys.stderr)
        return None


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", required=True)
    parser.add_argument("--base", default="")
    parser.add_argument("--head", default="HEAD")
    args = parser.parse_args()
    print(json.dumps(select(changed_paths(args.event, args.base, args.head)), separators=(",", ":")))
