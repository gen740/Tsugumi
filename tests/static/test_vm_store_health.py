"""Guest boot decision and durable marker contract, without Nix or a VM."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "vm/guest/store-health.sh"
# A Nix build or container may have no /bin/cat.
REAL_CAT = shutil.which("cat") or "/bin/cat"


def guest(tmp_path, monkeypatch):
    tools = tmp_path / "bin"
    tools.mkdir()
    commands = {
        "sync": ('printf "sync %s marker=%s\\n" "$*" "$(test -e "$HEALTH/running" && echo present || echo absent)" >> "$CALLS"; '
                 '[ "${FAIL_SYNC:-never}" != "$*" ]'),
        "nix-store": 'printf "nix-store %s\\n" "$*" >> "$CALLS"; echo "repaired /nix/store/example"; exit "${CHECK_STATUS:-0}"',
        # Never signal a real daemon on the machine running this test.
        "cat": 'case "$1" in /proc/*/comm) exit 1 ;; esac; exec ' + REAL_CAT + ' "$@"',
    }
    for name, code in commands.items():
        file = tools / name
        file.write_text("#!/bin/sh\n" + code + "\n")
        file.chmod(0o755)
    monkeypatch.setenv("PATH", str(tools) + os.pathsep + os.environ["PATH"])
    monkeypatch.setenv("CALLS", str(tmp_path / "calls"))
    persist = tmp_path / "persist"
    monkeypatch.setenv("HEALTH", str(persist / "store-health"))
    def run(action, *args):
        return subprocess.run(["bash", str(SCRIPT), action, str(persist), *args],
                              capture_output=True, text=True)
    return persist / "store-health", run


def test_clean_boot_skips_but_unclean_boot_repairs(tmp_path, monkeypatch):
    health, run = guest(tmp_path, monkeypatch)
    assert run("begin").stdout.strip() == ""
    assert (health / "running").exists()
    assert run("check", "").returncode == 0
    assert "nix-store" not in (tmp_path / "calls").read_text()
    assert run("poweroff").returncode == 0
    assert not (health / "running").exists()
    assert run("begin").stdout.strip() == ""
    assert run("begin").stdout.strip() == "unclean"
    assert run("check", "unclean").returncode == 0
    assert "repaired /nix/store/example" in (health / "check.log").read_text()
    assert "nix-store --verify --check-contents --repair" in (tmp_path / "calls").read_text()


def test_failed_backing_filesystem_sync_retains_marker(tmp_path, monkeypatch):
    health, run = guest(tmp_path, monkeypatch)
    assert run("begin").returncode == 0
    assert run("check", "unclean").returncode == 0
    monkeypatch.setenv("FAIL_SYNC", f"-f {health.parent}")
    assert run("poweroff").returncode != 0
    assert (health / "running").exists()
    monkeypatch.delenv("FAIL_SYNC")
    assert run("begin").stdout.strip() == "unclean"




@pytest.mark.parametrize("clean_stop", [False, True])
def test_failed_repair_retains_dirty_marker_and_retries(tmp_path, monkeypatch, clean_stop):
    health, run = guest(tmp_path, monkeypatch)
    run("begin")
    monkeypatch.setenv("CHECK_STATUS", "7")
    result = run("check", "unclean")
    assert result.returncode == 7
    assert "projects were not started" in result.stdout
    assert (health / "running").exists()
    assert run("begin").stdout.strip() == "unclean"
    reason = "unclean"
    if clean_stop:
        assert run("poweroff").returncode == 0
        reason = ""
        assert run("begin").stdout.strip() == reason
    monkeypatch.setenv("CHECK_STATUS", "0")
    assert run("check", reason).returncode == 0


def test_boot_marks_before_writes_and_repairs_before_projects():
    boot = (ROOT / "vm/guest/vm-boot.nix").read_text()
    assert boot.index('storeHealth} begin') < boot.index('nix-store --load-db')
    assert boot.index('storeHealth} check') < boot.index('stage supervisor')
    assert '|| fail "Nix store content check/repair failed' in boot
