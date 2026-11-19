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


@pytest.mark.parametrize("path", [
    "protocol/host.fbs", "flake.lock", "flake.nix", "nix/core.nix", "CMakeLists.txt",
    "tsugumid/CMakeLists.txt", "sdk/CMakeLists.txt",
    ".github/workflows/docs.yml", ".github/actions/setup-nix/action.yml",
    "tests/ci/select_jobs.py", "tests/conftest.py", "tests/fixtures/agent-contract.json",
    "unknown/new-file", "docs-with-a-new-name/Usage.md",
])
def test_shared_or_unclassified_paths_run_everything(path):
    assert selection.select([path]) == selection.select()


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
