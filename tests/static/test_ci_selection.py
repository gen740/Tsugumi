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
    selected = selection.select(paths)
    matrix = {
        entry["system"]: {check for job in entry["tests"]["include"] for check in job["checks"]}
        for entry in selected["matrix"]["include"]
    }
    if selected["vm"] or selected["pack-guest"]:
        matrix["aarch64-linux"] = {job for job in selection.GUEST if selected[job]}
    return matrix


@pytest.mark.parametrize("path", [
    "protocol/host.fbs", "flake.lock", "flake.nix", "nix/core.nix", "CMakeLists.txt",
    "tsugumid/CMakeLists.txt", "sdk/CMakeLists.txt",
    ".github/workflows/docs.yml", ".github/actions/setup-nix/action.yml",
    "tests/ci/select_jobs.py", "tests/conftest.py", "tests/fixtures/agent-contract.json",
    "unknown/new-file", "docs-with-a-new-name/Usage.md",
])
def test_shared_or_unclassified_paths_run_everything(path):
    assert selection.select([path]) == selection.select()


def test_full_run_schedules_only_supported_jobs():
    plan = selection.select()
    assert {entry["system"]: [job["job"] for job in entry["tests"]["include"]]
            for entry in plan["matrix"]["include"]} == {
        "x86_64-linux": ["native-checks", "host-and-web", "sdk", "remote-execution", "linux-ui"],
        "aarch64-darwin": ["native-checks", "swift"],
    }
    assert checks(None)["aarch64-linux"] == {"vm", "pack-guest"}


def test_changed_paths_produce_only_selected_matrix_entries():
    assert not selection.select(["README.md"])["platform"]
    assert checks(["ui/macos/Package.swift"]) == {"aarch64-darwin": {"swift"}}
    assert checks(["tests/remote/test_worker.py"]) == {"x86_64-linux": {"remote-execution"}}
    assert checks(["pack/build.sh"]) == {"aarch64-linux": {"pack-guest"}}
    assert checks(["ui/linux/src/main.cc"]) == {"x86_64-linux": {"linux-ui"}}
    assert checks(["tests/core/agent/project_rpc_test.cc"])["aarch64-linux"] == {"vm"}

    boundary = selection.select(["ui/web/scripts/check-boundaries.mjs"])
    assert boundary["web-typecheck"] and not boundary["platform"]
    web = selection.select(["ui/web/src/App.tsx"])
    assert web["web-typecheck"]
    assert checks(["ui/web/src/App.tsx"]) == {
        "x86_64-linux": {"adapters", "web-contracts"},
    }
    assert all(
        job["checks"] and all(job["checks"].values())
        for plan in (web, selection.select())
        for entry in plan["matrix"]["include"]
        for job in entry["tests"]["include"]
    )


def test_a_shared_native_test_fixture_selects_all_its_consumers():
    selected = checks(["tests/core/doubles/nix_eval.cc"])
    assert {"swift", "rpc", "agent-rpc"} <= selected["aarch64-darwin"]
    assert {"adapters", "sdk"} <= selected["x86_64-linux"]


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
