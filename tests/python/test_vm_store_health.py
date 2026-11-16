"""Guest boot decision and durable marker contract, without Nix or a VM."""
import os
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "macOS/Container/store-health.sh"


def guest(tmp_path, monkeypatch):
    tools = tmp_path / "bin"
    tools.mkdir()
    commands = {
        "sync": ('printf "sync %s marker=%s\\n" "$*" "$(test -e "$HEALTH/running" && echo present || echo absent)" >> "$CALLS"; '
                 '[ "${FAIL_SYNC:-never}" != "$*" ]'),
        "nix-store": 'printf "nix-store %s\\n" "$*" >> "$CALLS"; echo "repaired /nix/store/example"; exit "${CHECK_STATUS:-0}"',
        # The overlay on /nix is never remounted: the kernel refuses it.
        "mount": ('printf "mount %s\\n" "$*" >> "$CALLS"; '
                  'case "$3" in /history-disk) [ "$3" != "${BUSY_MOUNT:-}" ] ;; '
                  '*) echo "mount: target is busy" >&2; exit 32 ;; esac'),
        # Never signal a real daemon on the machine running this test.
        "cat": 'case "$1" in /proc/*/comm) exit 1 ;; esac; exec /bin/cat "$@"',
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


def test_existing_disk_checks_once_and_clean_boot_skips(tmp_path, monkeypatch):
    health, run = guest(tmp_path, monkeypatch)
    assert run("begin", "image-a").stdout.strip() == "upgrade"
    assert (health / "running").exists()
    checked = run("check", "image-a", "upgrade")
    assert checked.returncode == 0, checked.stderr
    assert "checkingStoreUpgrade" in checked.stdout
    assert "repaired /nix/store/example" in (health / "check.log").read_text()
    assert (health / "checked-version").read_text().strip() == "1"
    assert (health / "boot-closure").read_text().strip() == "image-a"
    before_stop = (tmp_path / "calls").read_text()
    stopped = run("poweroff")
    assert stopped.returncode == 0, stopped.stderr
    assert not (health / "running").exists()
    stop_calls = (tmp_path / "calls").read_text()[len(before_stop):].splitlines()
    assert stop_calls == [
        "sync  marker=present",
        "mount -o remount,ro /history-disk",
        "sync  marker=present",
        f"sync -f {health.parent} marker=present",
        f"sync -f {health.parent} marker=absent",
    ]
    assert run("begin", "image-a").stdout.strip() == ""
    before = (tmp_path / "calls").read_text()
    assert run("check", "image-a", "").returncode == 0
    assert (tmp_path / "calls").read_text() == before
    assert "nix-store --verify --check-contents --repair" in before


def test_failed_read_only_remount_retains_marker(tmp_path, monkeypatch, busy_mount="/history-disk"):
    health, run = guest(tmp_path, monkeypatch)
    assert run("begin", "image-a").returncode == 0
    assert run("check", "image-a", "upgrade").returncode == 0
    monkeypatch.setenv("BUSY_MOUNT", busy_mount)
    assert run("poweroff").returncode != 0
    assert (health / "running").exists()
    assert run("begin", "image-a").stdout.strip() == "unclean"


def test_failed_backing_filesystem_sync_retains_marker(tmp_path, monkeypatch):
    health, run = guest(tmp_path, monkeypatch)
    assert run("begin", "image-a").returncode == 0
    assert run("check", "image-a", "upgrade").returncode == 0
    monkeypatch.setenv("FAIL_SYNC", f"-f {health.parent}")
    assert run("poweroff").returncode != 0
    assert (health / "running").exists()
    monkeypatch.delenv("FAIL_SYNC")
    assert run("begin", "image-a").stdout.strip() == "unclean"


def test_dirty_boot_and_new_image_require_content_checks(tmp_path, monkeypatch):
    health, run = guest(tmp_path, monkeypatch)
    run("begin", "image-a")
    run("check", "image-a", "upgrade")
    assert run("begin", "image-a").stdout.strip() == "unclean"
    check = run("check", "image-a", "unclean")
    assert "TSUGUMI-STAGE checkingStore\n" in check.stdout
    run("clean")
    assert run("begin", "image-b").stdout.strip() == "image"
    run("clean")
    (health / "checked-version").write_text("old")
    assert run("begin", "image-a").stdout.strip() == "upgrade"


@pytest.mark.parametrize("clean_stop", [False, True])
def test_failed_repair_retains_dirty_marker_and_retries(tmp_path, monkeypatch, clean_stop):
    health, run = guest(tmp_path, monkeypatch)
    run("begin", "image-a")
    monkeypatch.setenv("CHECK_STATUS", "7")
    result = run("check", "image-a", "upgrade")
    assert result.returncode == 7
    assert "projects were not started" in result.stdout
    assert (health / "running").exists()
    assert not (health / "checked-version").exists()
    assert run("begin", "image-a").stdout.strip() == "unclean"
    reason = "unclean"
    if clean_stop:
        assert run("poweroff").returncode == 0
        reason = "upgrade"
        assert run("begin", "image-a").stdout.strip() == reason
    monkeypatch.setenv("CHECK_STATUS", "0")
    assert run("check", "image-a", reason).returncode == 0
    assert (health / "checked-version").read_text().strip() == "1"


def test_boot_marks_before_writes_and_repairs_before_projects():
    boot = (ROOT / "macOS/Container/vm-boot.nix").read_text()
    assert boot.index('storeHealth} begin') < boot.index('nix-store --load-db')
    assert boot.index('storeHealth} check') < boot.index('stage system')
    assert '|| fail "Nix store content check/repair failed' in boot
    assert 'fsync-store-paths = true' in boot
    assert 'fsync-metadata = true' in boot
