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
DEMOS_PIN = "flake.lock:tsugumi-demos"
NATIVE_TESTS = {"agent-rpc", "rpc"}
CORE = NATIVE_TESTS | {"host", "remote-execution", "adapters", "swift", "vm", "kit", "view-abi"}
PACK = {"pack-native"}
WEB = {"docs", "web-typecheck", "web-contracts", "adapters"}
ALL = CORE | PACK | WEB | {"demos", "prompt"}
ADAPTER_TESTS = {
    "tests/python/test_claude_code_plugin.py",
    "tests/python/test_web_server.py",
    "tests/python/test_cli.py",
}
WEB_AGENT_ASSETS = {
    "agent.json", "demo-skill.md", "generate.py", "mcp-transport.md", "system-prompt.md",
}


# nix/core-source.nix's fileset without its Darwin extras. With nix/,
# flake.{nix,lock} and vm/guest/ (which select the vm job already) these name
# the guest image's identity (nix/guest-image.nix): a change to any of them
# needs a new image, which only the vm job builds and publishes.
GUEST_SOURCES = (
    "VERSION", "CMakeLists.txt", "cmake/", "workerd/", "ui/shared/i18n/en.json", "util/", "rpc/",
    "tsugumid/", "hostd/CMakeLists.txt", "hostd/cmake/", "hostd/src/", "hostd/cache/", "hostd/private/",
    "hostd/agent/src/", "hostd/agent/private/", "vm/guest/CMakeLists.txt", "vm/guest/src/",
    "client/CMakeLists.txt", "client/cmake/", "client/include/", "client/src/", "cli/CMakeLists.txt",
    "cli/src/", "nix/card-entry.nix", "nix/worker.nix", "protocol/CMakeLists.txt", "protocol/include/",
    "protocol/trace_codec.cc", "hostd/agent/Shared/system-prompt.md", "hostd/agent/Shared/guide-prompt.md",
    "hostd/agent/Shared/demo-skill.md", "hostd/agent/Shared/helpers-skill.md",
    "hostd/agent/Shared/review-skill.md", "hostd/agent/Shared/rerun-compare-skill.md",
    "hostd/agent/Shared/project-files.md",
    "hostd/agent/Shared/agent.json", "hostd/agent/Shared/guide-script.json",
    "hostd/agent/Shared/guide-script.en.json",
)


def guest_source(path):
    if path.startswith("protocol/") and path.endswith(".fbs"):
        return True
    return any(path.startswith(entry) if entry.endswith("/") else path == entry for entry in GUEST_SOURCES)


def consumers(path):
    jobs = path_consumers(path)
    return jobs | {"vm"} if guest_source(path) else jobs


def path_consumers(path):
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
        if path == "tests/core/harness/demos.py":
            return {"rpc", "adapters", "demos"}
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
        if path in {"docs/features.md", "docs/design/Agent.md"}:
            jobs.add("prompt")
        return jobs
    if path.startswith("vm/guest/"):
        if path.startswith("vm/guest/tools/"):
            return {"vm"}
        return {"host", "vm"}
    if path.startswith("pack/"):
        return PACK
    if path.startswith(("ui/macos/", "vm/macos/", "client/swift/", "cli/vmctl/")):
        return {"swift"} | (PACK if path.startswith("ui/macos/View") else set())
    if path.startswith(("ui/web/tests/", "ui/web/scripts/test.mjs")):
        return {"web-contracts"}
    if path == "ui/web/scripts/check-boundaries.mjs":
        return {"web-typecheck"}
    if path.startswith("ui/web/"):
        return WEB
    if path == DEMOS_PIN:
        # Guide and version tests, demo runs, the MCP plugin, and the Web player.
        return {"docs", "web-contracts", "adapters", "demos", "agent-rpc", "rpc"}
    if path.startswith("ui/shared/dawn/"):
        # The dawn view generator: its own check, the Mac host and the Web shim.
        return {"view-abi", "swift"} | PACK | WEB
    if path.startswith("ui/shared/"):
        return CORE | PACK | WEB | {"demos"}
    if path.startswith("client/"):
        # Shared native client, Swift C interface and the browser Wasm build.
        return CORE | PACK | WEB | {"demos"}
    if path.startswith(("util/", "rpc/", "tsugumid/document/")):
        return CORE | PACK | WEB | {"demos"}
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
    if path.startswith("hostd/cache/"):
        # The cache server is its own process, tested by the host suite.
        return {"host"}
    if path.startswith("hostd/"):
        # Host RPC is consumed by native clients, the broker and the Web edit
        # driver. Guest entry points and Card evaluation run through tsugumid.
        return {"agent-rpc", "rpc", "host", "remote-execution", "adapters", "swift", "web-contracts"}
    if path.startswith(("tsugumid/", "workerd/")):
        return CORE | PACK
    if path.startswith("cli/"):
        return {"adapters", "swift"}
    if path.startswith("plugins/") or path in ADAPTER_TESTS:
        return {"adapters"}
    if path == "tests/python/test_host_projects_rpc.py":
        return {"agent-rpc"}
    if path.startswith("tests/python/"):
        if path.rsplit("/", 1)[1].startswith("test_demo") or path == "tests/python/test_collatz_compare.py":
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


STANDALONE = {"docs", "prompt", "web-typecheck", "vm"} | PACK


def select(paths=None):
    changes = [(path, consumers(path)) for path in paths] if paths else [("", ALL)]
    jobs = set().union(*(selected for _, selected in changes))
    matrix = []
    host_checks = {"host", "adapters", "web-contracts"}
    for system, groups in {
        # Linux's host integrations take several minutes. Keep them and their
        # Web consumers parallel to the cold C++ test build and RPC suites.
        "x86_64-linux": {"native-checks": NATIVE_TESTS | {"view-abi"}, "host-and-web": host_checks,
                         "kit-and-demos": {"kit", "demos"}, "remote-execution": {"remote-execution"}},
        "aarch64-darwin": {"native-checks": NATIVE_TESTS | {"host", "view-abi"}, "swift": {"swift"},
                           "kit": {"kit"}, "demos": {"demos"}},
    }.items():
        selected = set().union(*(jobs for path, jobs in changes if system in systems_for(path)))
        if system == "aarch64-darwin" and any(path == DEMOS_PIN for path, _ in changes):
            selected.add("demos")
        lanes = []
        for job, checks in groups.items():
            if suites := selected & checks:
                lanes.append({"job": job, "checks": {suite: True for suite in sorted(suites)}})
        if lanes:
            matrix.append({"system": system, "runner": SYSTEMS[system], "tests": {"include": lanes}})
    return {**{job: job in jobs for job in sorted(STANDALONE)},
            "platform": bool(matrix), "matrix": {"include": matrix}}


def systems_for(path):
    if path in {
        DEMOS_PIN,
        "tests/python/test_host_projects_rpc.py",
    }:
        return {"x86_64-linux"}
    if path.startswith("tests/core/test_") and path.endswith(".py"):
        file = path.rsplit("/", 1)[1]
        if file not in NATIVE_CORE_FILES and file not in NATIVE_CORE_CASES:
            return {"x86_64-linux"}
    return SYSTEMS.keys()


def without_demos_input(lock):
    nodes = lock["nodes"]
    name = nodes[lock["root"]]["inputs"].pop("tsugumi-demos")
    node = nodes.pop(name)
    if node.get("flake") is not False or node.get("inputs"):
        raise ValueError("tsugumi-demos must be a non-flake leaf")
    return lock, node


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
        if not result.stdout:
            return None
        paths = result.stdout.decode("utf-8").rstrip("\0").split("\0")
        if "flake.lock" in paths:
            try:
                before = base
                if event == "pull_request":
                    before = subprocess.check_output(["git", "merge-base", base, head], text=True).strip()
                old, old_pin = without_demos_input(
                    json.loads(subprocess.check_output(["git", "show", f"{before}:flake.lock"]))
                )
                new, new_pin = without_demos_input(
                    json.loads(subprocess.check_output(["git", "show", f"{head}:flake.lock"]))
                )
                if old == new and old_pin != new_pin:
                    paths[paths.index("flake.lock")] = DEMOS_PIN
            except (subprocess.CalledProcessError, KeyError, TypeError, ValueError):
                # A missing or unrecognized lock stays a shared dependency.
                pass
        return paths
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
