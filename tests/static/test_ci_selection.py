"""CI selection must cover consumers, preserve prerequisites and fail open."""

import importlib.util
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ci_selection", ROOT / "tests/ci/select_jobs.py")
selection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(selection)


def checks(paths):
    return {entry["system"]: {job for job, run in entry["checks"].items() if run}
            for entry in selection.select(paths)["matrix"]["include"]}


def test_prose_skips_platform_builds_but_checks_documented_contracts():
    assert checks(["docs/Usage.md", "README.md"]) == {}
    assert selection.select(["docs/Usage.md"])["docs"]
    assert selection.select(["docs/features.md"])["prompt"]
    guide = checks(["docs/CardGuide.md"])
    assert guide == {
        "x86_64-linux": {"sdk", "native", "web", "web-contracts"},
        "aarch64-linux": {"sdk", "native"},
    }
    assert checks(["docs/site/src/content.config.ts"]) == {}


def test_demos_cover_guide_sdk_adapters_versions_and_player_on_one_platform():
    path = "demos/ja/casa/definitions/bands/src/main.py"
    result = selection.select([path])
    assert result["docs"] and not result["prompt"]
    assert checks([path]) == {
        "x86_64-linux": {"native", "web", "agent-rpc", "rpc", "sdk", "adapters", "web-contracts"},
    }
    combined = checks([path, "ui/macos/Package.swift", "workerd/src/native_main.cc"])
    assert "swift" in combined["aarch64-darwin"]
    assert "sdk" in combined["aarch64-linux"]


@pytest.mark.parametrize("path", [
    "ui/web/src/ui/demoPlayer.tsx", "docs/site/player/build.mjs",
    "docs/site/player-data/assets/casa.run.json", "ui/shared/templates/marimo.json",
    "sdk/python/tsugumi/contract.py", "client/src/client.cc",
])
def test_generated_player_consumers_run_together(path):
    result = selection.select([path])
    assert result["docs"]
    assert checks([path])["x86_64-linux"] >= {"web", "web-contracts"}


def test_swift_and_tooling_select_only_their_platform_dependencies():
    assert checks(["ui/macos/Sources/App/Home/DemoThumbnail.swift"]) == {
        "aarch64-darwin": {"native", "swift"},
    }
    assert checks(["plugins/tsugumi/bin/tsugumi-mcp"]) == {
        system: {"native", "web", "adapters"} for system in ("x86_64-linux", "aarch64-linux")
    }
    assert checks(["cli/install.sh"])["aarch64-darwin"] == {"native", "swift"}


@pytest.mark.parametrize("path", [
    "protocol/host.fbs", "flake.lock", "flake.nix", "nix/core.nix", "CMakeLists.txt",
    "tsugumid/CMakeLists.txt", "sdk/CMakeLists.txt",
    ".github/workflows/docs.yml", ".github/actions/setup-nix/action.yml",
    "tests/ci/select_jobs.py", "tests/conftest.py", "tests/fixtures/agent-contract.json",
    "unknown/new-file", "docs-with-a-new-name/Usage.md",
])
def test_shared_or_unclassified_paths_run_everything(path):
    assert selection.select([path]) == selection.select()


def test_native_changes_cover_all_platform_runtime_consumers():
    for path in ("tsugumid/runtime/src/service.cc", "workerd/src/native_main.cc"):
        result = checks([path])
        assert result["x86_64-linux"] >= {"rpc", "host", "remote-execution", "sdk", "adapters"}
        assert {"vm", "pack-guest"} <= result["aarch64-linux"]
        assert "swift" in result["aarch64-darwin"]


def test_agent_consumers_depend_on_shared_assets_separately_from_native_code():
    path = "hostd/agent/src/runtime.cc"
    assert checks([path]) == {
        "x86_64-linux": {"native", "web", "agent-rpc", "adapters"},
        "aarch64-linux": {"native", "web", "agent-rpc", "adapters"},
        "aarch64-darwin": {"native", "agent-rpc", "swift"},
    }
    path = "hostd/agent/Shared/agent.json"
    assert "web-contracts" in checks([path])["x86_64-linux"]
    assert not any({"vm", "pack-guest", "sdk"} & jobs for jobs in checks([path]).values())
    assert selection.select(["hostd/agent/Shared/helpers-skill.md"])["prompt"]


def test_core_tests_and_host_only_sources_skip_guest_and_sdk_builds():
    for path, job in (
        ("tests/core/test_permissions.py", "rpc"),
        ("tests/core/cmake/installed-core.cmake", "rpc"),
        ("tests/core/integration/test_definition_evaluation.py", "host"),
        ("hostd/src/project_operations.cc", "agent-rpc"),
    ):
        result = checks([path])
        assert job in result["x86_64-linux"]
        assert not any({"vm", "pack-guest", "sdk"} & jobs for jobs in result.values())
    assert checks(["tests/core/vm_lifetime_test.cc"]) == {
        "aarch64-linux": {"native", "vm"},
    }
    assert checks(["tests/core/test_permissions.py"]) == {
        "x86_64-linux": {"native", "rpc"},
    }
    assert set(checks(["tests/core/test_project_shell.py"])) == set(selection.SYSTEMS)
    assert checks(["tests/core/integration/test_definition_evaluation.py"]) == {
        "x86_64-linux": {"native", "host"},
    }
    for path in ("tests/python/conftest.py", "tests/core/harness/file_events.py"):
        assert "agent-rpc" in checks([path])["x86_64-linux"]


@pytest.mark.parametrize("path", [
    "vm/guest/tools/vm-artifact.py", "vm/guest/tools/vm-download.py", "vm/guest/tools/vm-snapshot.py",
])
def test_guest_tools_select_only_the_release_image(path):
    assert checks([path]) == {
        "aarch64-linux": {"native", "vm"},
    }


def test_guest_kernel_changes_select_both_images():
    assert checks(["vm/guest/kernel.nix"])["aarch64-linux"] == {
        "native", "vm", "pack-guest", "host",
    }


def test_pack_changes_build_only_the_pack_guest():
    assert checks(["pack/guest/default.nix"]) == {"aarch64-linux": {"pack-guest"}}


def test_every_selected_job_has_its_build_prerequisites_on_supported_platforms():
    paths = list(selection.ADAPTER_TESTS) + [
        "README.md", "docs/CardGuide.md", "docs/site/src/content.config.ts",
        "vm/guest/vm-system.nix", "ui/macos/Package.swift", "ui/web/package.json",
        "demos/en/index.json", "ui/shared/modules/viewer.js", "sdk/src/card_runtime.cc",
        "client/src/client.cc", "tsugumid/runtime/src/service.cc", "hostd/agent/Shared/agent.json",
        "cli/install.ps1", "plugins/tsugumi/bin/tsugumi-mcp", "tests/python/conftest.py",
        "tests/remote/test_worker.py", "tests/prompt/inspect_probe.py", "pack/build.sh", "new-path",
    ]
    for path in paths:
        for system, jobs in checks([path]).items():
            if jobs & selection.CORE:
                assert "native" in jobs, (path, system)
            if jobs & {"web-contracts", "adapters"}:
                assert "web" in jobs, (path, system)
            if system == "aarch64-darwin":
                assert not jobs & {"web", "sdk", "adapters", "web-contracts", "vm", "pack-guest", "remote-execution"}
            if jobs & {"vm", "pack-guest"}:
                assert system == "aarch64-linux"
            if "remote-execution" in jobs:
                assert system == "x86_64-linux"


def test_scheduled_manual_missing_base_and_initial_push_run_all(monkeypatch):
    def no_diff(*args, **kwargs):
        pytest.fail("A full run must not depend on a diff")
    monkeypatch.setattr(subprocess, "run", no_diff)
    for event, base in [("schedule", "abc"), ("workflow_dispatch", "abc"),
                        ("push", ""), ("push", "0" * 40)]:
        assert selection.select(selection.changed_paths(event, base, "HEAD")) == selection.select()


def test_diff_handles_both_sides_of_renames_and_unusual_filenames(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    def git(*args):
        return subprocess.check_output(
            ["git", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", *args],
            text=True,
        ).strip()

    git("init", "--quiet")
    old = Path("docs/old name.md")
    new = Path("core/new\nname.cc")
    old.parent.mkdir()
    new.parent.mkdir()
    old.write_text("fixture")
    git("add", str(old))
    git("commit", "--quiet", "-m", "Before rename")
    base = git("rev-parse", "HEAD")
    git("mv", str(old), str(new))
    git("commit", "--quiet", "-m", "Rename fixture")
    paths = selection.changed_paths("pull_request", base, "HEAD")
    assert set(paths) == {str(old), str(new)}
    assert selection.select(paths)["docs"] and "rpc" in checks(paths)["x86_64-linux"]


def test_empty_or_failed_push_diff_runs_all(monkeypatch):
    def diff(command, **kwargs):
        assert "before" in command and "head" in command
        return subprocess.CompletedProcess(command, 0, stdout=b"")
    monkeypatch.setattr(subprocess, "run", diff)
    assert selection.changed_paths("push", "before", "head") is None
    def failed(*args, **kwargs):
        raise subprocess.CalledProcessError(128, "git diff")
    monkeypatch.setattr(subprocess, "run", failed)
    assert selection.changed_paths("push", "before", "head") is None


def test_ci_scripts_do_not_shadow_standard_modules():
    # A script's own folder is first on sys.path: a file named like a standard
    # module replaces it for every other script in that folder.
    import sys

    shadowing = sorted(p.name for p in (ROOT / "tests/ci").glob("*.py") if p.stem in sys.stdlib_module_names)
    assert shadowing == []
