"""CI selection must cover consumers, preserve prerequisites and fail open."""

import importlib.util
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ci_selection", ROOT / "tests/ci/select.py")
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
    assert "sdk" in guide["x86_64-linux"]
    assert "swift" in guide["aarch64-darwin"]


def test_demos_cover_guide_rpc_sdk_web_and_swift_without_unrelated_core_tests():
    result = selection.select(["demos/ja/casa/definitions/bands/src/main.py"])
    assert result["docs"] and not result["prompt"]
    for entry in result["matrix"]["include"]:
        jobs = entry["checks"]
        assert jobs["native"] and jobs["agent-rpc"] and not jobs["rpc"]
        if entry["system"].endswith("linux"):
            assert jobs["sdk"] and jobs["web"] and jobs["web-contracts"] and jobs["adapters"]
        else:
            assert jobs["swift"]


@pytest.mark.parametrize("path", [
    "web/src/ui/demoPlayer.tsx", "docs/site/player/build.mjs",
    "docs/site/player-data/assets/casa.run.json", "ui/templates/marimo.json",
    "sdk/python/tsugumi/contract.py", "client/src/client.cc",
])
def test_generated_player_consumers_run_together(path):
    result = selection.select([path])
    assert result["docs"]
    assert checks([path])["x86_64-linux"] >= {"web", "web-contracts"}
    assert "swift" in checks([path])["aarch64-darwin"]


def test_swift_and_tooling_select_only_their_platform_dependencies():
    assert checks(["macOS/Sources/App/Home/DemoThumbnail.swift"]) == {
        "aarch64-darwin": {"native", "swift"},
    }
    assert checks(["plugins/tsugumi/bin/tsugumi-mcp"]) == {
        system: {"native", "web", "adapters"} for system in ("x86_64-linux", "aarch64-linux")
    }
    assert checks(["cli/install.sh"])["aarch64-darwin"] == {"native", "swift"}


@pytest.mark.parametrize("path", [
    "protocol/host.fbs", "flake.lock", "flake.nix", "nix/core.nix", "CMakeLists.txt",
    "core/CMakeLists.txt", "sdk/CMakeLists.txt", "tests/core/cmake/installed-core.cmake",
    ".github/workflows/docs.yml", ".github/actions/setup-nix/action.yml",
    "tests/ci/select.py", "tests/conftest.py", "tests/fixtures/agent-contract.json",
    "unknown/new-file", "docs-with-a-new-name/Usage.md",
])
def test_shared_or_unclassified_paths_run_everything(path):
    assert selection.select([path]) == selection.select()


def test_native_changes_cover_all_platform_runtime_consumers():
    for path in ("core/src/service.cc", "workerd/src/native_main.cc"):
        result = checks([path])
        assert result["x86_64-linux"] >= {"rpc", "host", "remote-execution", "sdk", "adapters"}
        assert "vm" in result["aarch64-linux"]
        assert "swift" in result["aarch64-darwin"]


def test_every_selected_job_has_its_build_prerequisites_on_supported_platforms():
    paths = list(selection.ADAPTER_TESTS) + [
        "README.md", "docs/CardGuide.md", "docs/site/src/content.config.ts",
        "macOS/Container/vm-system.nix", "macOS/Package.swift", "web/package.json",
        "demos/en/index.json", "ui/modules/viewer.js", "sdk/src/card_runtime.cc",
        "client/src/client.cc", "core/src/service.cc", "agent/Shared/agent.json",
        "cli/install.ps1", "plugins/tsugumi/bin/tsugumi-mcp", "tests/python/conftest.py",
        "tests/remote/test_worker.py", "tests/prompt/inspect_probe.py", "new-path",
    ]
    for path in paths:
        for system, jobs in checks([path]).items():
            if jobs & selection.CORE:
                assert "native" in jobs, (path, system)
            if jobs & {"web-contracts", "adapters"}:
                assert "web" in jobs, (path, system)
            if system == "aarch64-darwin":
                assert not jobs & {"web", "sdk", "adapters", "web-contracts", "vm", "remote-execution"}
            if "vm" in jobs:
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


def test_diff_handles_both_sides_of_renames_and_unusual_filenames(monkeypatch):
    def diff(command, **kwargs):
        assert command == ["git", "diff", "--name-only", "--no-renames", "-z", "base...head", "--"]
        return subprocess.CompletedProcess(command, 0, stdout=b"docs/old name.md\0core/new\nname.cc\0")
    monkeypatch.setattr(subprocess, "run", diff)
    paths = selection.changed_paths("pull_request", "base", "head")
    assert paths == ["docs/old name.md", "core/new\nname.cc"]
    assert selection.select(paths)["docs"] and "rpc" in checks(paths)["x86_64-linux"]


def test_empty_or_failed_push_diff_runs_all(monkeypatch):
    def diff(command, **kwargs):
        assert command[-3:] == ["before", "head", "--"]
        return subprocess.CompletedProcess(command, 0, stdout=b"")
    monkeypatch.setattr(subprocess, "run", diff)
    assert selection.changed_paths("push", "before", "head") is None
    def failed(*args, **kwargs):
        raise subprocess.CalledProcessError(128, "git diff")
    monkeypatch.setattr(subprocess, "run", failed)
    assert selection.changed_paths("push", "before", "head") is None
