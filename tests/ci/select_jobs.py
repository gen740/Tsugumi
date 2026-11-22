"""Select CI consumers of changed paths; unknown paths and uncertain diffs run all.

The source filesets in nix/{core,core-tests,client-wasm,web,web-checks}.nix,
the CMake install rules and ci-platform.yml's test commands define this map.
Keep dependencies here, rather than maintaining path filters in each workflow.
"""

import argparse
import json
import subprocess
import sys

from core_platforms import NATIVE_CORE_CASES, NATIVE_CORE_FILES


SYSTEMS = {
    "x86_64-linux": "ubuntu-24.04",
    "aarch64-darwin": "macos-26",
}
NATIVE_TESTS = {"agent-rpc", "rpc"}
CORE = NATIVE_TESTS | {"host", "remote-execution", "adapters", "swift", "vm", "kit"}
GUEST = {"vm", "pack-guest"}
WEB = {"docs", "web-typecheck", "web-contracts", "adapters"}
ALL = CORE | GUEST | WEB | {"demos", "prompt"}
ADAPTER_TESTS = {
    "tests/python/test_claude_code_plugin.py",
    "tests/python/test_web_server.py",
    "tests/python/test_cli.py",
}
WEB_AGENT_ASSETS = {
    "agent.json", "demo-skill.md", "generate.py", "mcp-transport.md", "system-prompt.md",
}


def consumers(path):
    # Repository metadata (funding, issue templates) is not built or tested.
    if path.startswith(".github/") and not path.startswith((".github/workflows/", ".github/actions/")):
        return set()
    if path.startswith("tests/core/"):
        if path in {"tests/core/vm_lifetime_test.cc", "tests/core/cmake/vm.cmake"}:
            return {"vm"}
        if path in {"tests/core/doubles/nix_eval.cc", "tests/core/cmake/daemon-double.cmake"}:
            return NATIVE_TESTS | {"swift", "adapters"}
        if path in {"tests/core/input_types_test.cc", "tests/core/cmake/input-types.cmake"}:
            return {"agent-rpc"}
        if path in {"tests/core/agent/document_edit_driver.cc", "tests/core/cmake/host-edit-driver.cmake"}:
            return {"web-contracts"}
        if path in {"tests/core/CMakeLists.txt", "tests/core/cmake/installed-core.cmake"} or path.startswith(
            "tests/core/cmake/installed/"
        ):
            return NATIVE_TESTS | {"swift", "adapters", "vm", "web-contracts"}
        if path in {
            "tests/core/agent/project_rpc_test.cc", "tests/core/agent/project_fixture.hh",
            "tests/core/agent/fixture.hh", "tests/core/agent/view.hh",
            "tests/core/agent_fixture_server.cc", "tests/core/agent_fixture_server.hh",
            "tests/core/cmake/host-rpc.cmake",
        }:
            return NATIVE_TESTS | {"vm"}
        if path == "tests/core/test_system.py":
            return {"rpc", "vm"}
        if path.endswith((".cc", ".hh", ".cmake", "/CMakeLists.txt")):
            return NATIVE_TESTS
        if path.startswith("tests/core/integration/"):
            return {"host", "remote-execution"} if path.endswith("/test_remote_execution.py") else {"host"}
        # Shared harness modules also serve the Host project RPC and adapters.
        if path.startswith("tests/core/test_"):
            return {"rpc"}
        return {"agent-rpc", "rpc", "host", "remote-execution", "adapters", "vm"}
    if path.startswith(("protocol/", "nix/", ".github/", "tests/ci/")) or path in {
        "flake.nix", "flake.lock", "CMakeLists.txt",
    } or path.endswith(("/CMakeLists.txt", ".cmake")):
        return ALL
    if path in {"README.md", "tests/README.md"}:
        return {"docs"}
    if path.startswith("docs/"):
        jobs = {"docs"}
        # These are inputs to web.sitePlayer.
        if path.startswith(("docs/site/player/", "docs/site/player-data/assets/")) or path in {
            "docs/CardGuide.md", "docs/assets/tsugumi-icon.svg",
        }:
            jobs.add("web-contracts")
        if path == "docs/features.md":
            jobs.add("prompt")
        return jobs
    if path.startswith("vm/guest/"):
        if path.startswith("vm/guest/tools/"):
            return {"vm"}
        # Both guest images share the kernel and native runtime.
        return {"host", "vm", "pack-guest"}
    if path.startswith("pack/"):
        return {"pack-guest"}
    if path.startswith(("ui/macos/", "vm/macos/", "client/swift/", "cli/vmctl/")):
        return {"swift"}
    if path.startswith(("ui/web/tests/", "ui/web/scripts/test.mjs")):
        return {"web-contracts"}
    if path == "ui/web/scripts/check-boundaries.mjs":
        return {"web-typecheck"}
    if path.startswith("ui/web/"):
        return WEB
    if path.startswith("demos/"):
        # Guide and version tests, demo runs, the MCP plugin, and the Web player.
        return {"docs", "web-contracts", "adapters", "demos", "agent-rpc", "rpc"}
    if path.startswith("ui/shared/"):
        return CORE | GUEST | WEB | {"demos"}
    if path.startswith("client/"):
        # Shared native client, Swift C interface and the browser Wasm build.
        return CORE | GUEST | WEB | {"demos"}
    if path.startswith(("util/", "rpc/", "tsugumid/document/")):
        return CORE | GUEST | WEB | {"demos"}
    if path.startswith("hostd/agent/"):
        jobs = {"agent-rpc", "adapters", "swift"}
        if path.startswith("hostd/agent/Shared/"):
            asset = path.rsplit("/", 1)[1]
            if asset in WEB_AGENT_ASSETS:
                jobs.add("web-contracts")
            if asset in {"agent.json", "demo-skill.md"}:
                jobs.add("web-typecheck")
            if asset == "helpers-skill.md":
                jobs.add("prompt")
        return jobs
    if path.startswith("hostd/"):
        # Host RPC is consumed by native clients, the broker and the Web edit
        # driver. Guest entry points and Card evaluation run through tsugumid.
        return {"agent-rpc", "rpc", "host", "remote-execution", "adapters", "swift", "web-contracts"}
    if path.startswith(("tsugumid/", "workerd/")):
        return CORE | GUEST
    if path.startswith("cli/"):
        return {"adapters", "swift"}
    if path.startswith("plugins/") or path in ADAPTER_TESTS:
        return {"adapters"}
    if path == "tests/python/test_host_projects_rpc.py":
        return {"agent-rpc"}
    if path.startswith("tests/python/"):
        if path.rsplit("/", 1)[1].startswith("test_demo"):
            return {"demos"}
        # Includes shared adapter and demo harnesses.
        jobs = {"adapters"}
        if path == "tests/python/conftest.py":
            jobs |= {"agent-rpc", "demos"}
        return jobs
    if path.startswith("tests/remote/"):
        return {"remote-execution"}
    if path.startswith("tests/prompt/"):
        return {"prompt"}
    if path.startswith("tests/static/"):
        return set()
    # protocol/, flake.{nix,lock}, nix/, CMake, CI/actions/scripts, fixtures,
    # root configuration and every new/unclassified path deliberately run all.
    return ALL


STANDALONE = {"docs", "prompt", "web-typecheck"} | GUEST


def select(paths=None):
    changes = [(path, consumers(path)) for path in paths] if paths else [("", ALL)]
    jobs = set().union(*(selected for _, selected in changes))
    matrix = []
    host_checks = {"host", "adapters", "web-contracts"}
    for system, groups in {
        # Linux's host integrations take several minutes. Keep them and their
        # Web consumers parallel to the cold C++ test build and RPC suites.
        "x86_64-linux": {"native-checks": NATIVE_TESTS, "host-and-web": host_checks,
                         "kit-and-demos": {"kit", "demos"}, "remote-execution": {"remote-execution"}},
        "aarch64-darwin": {"native-checks": NATIVE_TESTS | {"host"}, "swift": {"swift"}, "kit": {"kit"}},
    }.items():
        selected = set().union(*(jobs for path, jobs in changes if system in systems_for(path)))
        lanes = []
        for job, checks in groups.items():
            if suites := selected & checks:
                lanes.append({"job": job, "checks": {suite: True for suite in sorted(suites)}})
        if lanes:
            matrix.append({"system": system, "runner": SYSTEMS[system], "tests": {"include": lanes}})
    return {**{job: job in jobs for job in sorted(STANDALONE)},
            "platform": bool(matrix), "matrix": {"include": matrix}}


def systems_for(path):
    if path.startswith(("demos/", "tests/python/test_demo")) or path in {
        "tests/python/test_host_projects_rpc.py",
    }:
        return {"x86_64-linux"}
    if path.startswith("tests/core/test_") and path.endswith(".py"):
        file = path.rsplit("/", 1)[1]
        if file not in NATIVE_CORE_FILES and file not in NATIVE_CORE_CASES:
            return {"x86_64-linux"}
    return SYSTEMS.keys()


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
    # A pull request stacked on another branch is checked again once it
    # targets main; until then only the static checks run.
    parser.add_argument("--stacked", action="store_true")
    args = parser.parse_args()
    selection = ({**{job: False for job in sorted(STANDALONE)}, "platform": False, "matrix": {"include": []}}
                 if args.stacked else select(changed_paths(args.event, args.base, args.head)))
    print(json.dumps(selection, separators=(",", ":")))
