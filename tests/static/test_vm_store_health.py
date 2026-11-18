"""Guest boot decision and durable marker contract, without Nix or a VM."""
import os
from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "vm/guest/store-health.sh"


def guest(tmp_path, monkeypatch):
    tools = tmp_path / "bin"
    tools.mkdir()
    commands = {
        "sync": ('printf "sync %s marker=%s\\n" "$*" "$(test -e "$HEALTH/running" && echo present || echo absent)" >> "$CALLS"; '
                 '[ "${FAIL_SYNC:-never}" != "$*" ]'),
        "nix-store": 'printf "nix-store %s\\n" "$*" >> "$CALLS"; echo "repaired /nix/store/example"; exit "${CHECK_STATUS:-0}"',
        # Wait for the fixture daemon exit rather than signal any real process.
        "vmd": 'test "$1" = --stop-nix && [ "${FAIL_STOP:-0}" = 0 ]',
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
        if action == "poweroff":
            args = (str(tools / "vmd"),)
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
    assert "repaired /nix/store/example" in (health / "check.log").read_text()
    assert (health / "running").exists()
    assert run("begin").stdout.strip() == "unclean"
    reason = "unclean"
    if clean_stop:
        assert run("poweroff").returncode == 0
        reason = ""
        assert run("begin").stdout.strip() == reason
    monkeypatch.setenv("CHECK_STATUS", "0")
    assert run("check", reason).returncode == 0


@pytest.mark.parametrize("repair_status", [0, 7], ids=["start-projects", "stop-on-failed-repair"])
def test_boot_marks_before_writes_and_repairs_before_projects(tmp_path, repair_status):
    boot = (ROOT / "vm/guest/vm-boot.nix").read_text()
    # Run the init script from disk setup to supervisor handoff. Point the
    # guest paths at fixtures and put only command doubles on PATH.
    init = boot.split("stage store\n", 1)[1].split("\n  '';", 1)[0]
    init = init.replace("''${", "${")
    tools = tmp_path / "tools"
    tools.mkdir()
    replacements = {
        "${storeHealth}": "health", "${vm-system}": "tools", "${busybox}/bin/": "tools/",
        "${udhcpcScript}": "fixture-dhcp",
    }
    for symbol, replacement in replacements.items():
        init = init.replace(symbol, replacement)
    init = re.sub(r"(?<![\w./])/((?:dev/vdb|persist|nix|run|mnt|registration|store-paths)\b)", r"\1", init)
    for directory in ("persist", "nix", "run", "mnt"):
        (tmp_path / directory).mkdir()
    for name in ("registration", "store-paths"):
        (tmp_path / name).touch()
    for command, body in {
        "health": 'printf "%s\n" "$1" >> calls\n'
                  'case "$1" in begin) echo unclean ;; check) exit "$REPAIR_STATUS" ;; esac',
        "nix-store": 'printf "nix-store %s\n" "$*" >> calls',
        "nix": 'printf "nix %s\n" "$*" >> calls',
        "nix-build": 'echo nix-build >> calls',
        "tini": 'printf "%s\n" "${2##*/}" >> calls',
        **dict.fromkeys(("blkid", "mount", "resize2fs", "ln", "df", "tail", "mkdir", "chmod", "awk",
                        "rm", "nix-daemon", "vmd", "du", "cut", "ip", "udhcpc", "wc"), ":"),
    }.items():
        stub = tools / command
        stub.write_text("#!/bin/sh\n" + body + "\n")
        stub.chmod(0o755)
    result = subprocess.run(
        [shutil.which("bash"), "-c", "stage() { :; }; fail() { exit 1; };\n" + init],
        cwd=tmp_path, env={**os.environ, "PATH": str(tools), "REPAIR_STATUS": str(repair_status)},
        capture_output=True, text=True,
    )
    calls = (tmp_path / "calls").read_text().splitlines()
    assert calls.index("begin") < calls.index("nix-store --load-db") < calls.index("check")
    assert "nix-build" not in calls
    assert all(call.startswith("nix config show ") for call in calls if call.startswith("nix "))
    if repair_status:
        assert result.returncode != 0 and "supervisor" not in calls
    else:
        assert result.returncode == 0, result.stderr
        assert calls.index("check") < calls.index("supervisor")
