"""The shared VM's nix.conf with the launcher's binary cache snapshot.

The guest's Nix configuration (macOS/Container/vm-boot.nix) includes the
snapshot `BinaryCaches.prepare` writes. Nix itself is asked what it would use,
with the snapshot at a temporary path: no VM, store or network is involved.
What the launcher writes is covered by macOS/CoreTests (BinaryCacheTests.swift);
a boot of the VM by tests/python/test_shared_vm.py."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import textwrap
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
GUEST = "/run/tsugumi/binary-caches"
DEFAULTS = ["https://cache.nixos.org", "https://tsugumi.cachix.org"]
KEY = "research:" + "BwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwcHBwc="

pytestmark = pytest.mark.skipif(shutil.which("nix") is None, reason="needs nix")


def guest_conf() -> str:
    source = (ROOT / "macOS/Container/vm-boot.nix").read_text()
    found = re.search(r'writeTextDir "etc/nix/nix\.conf" \'\'\n(.*?)\n  \'\';', source, re.S)
    assert found, "vm-boot.nix no longer defines etc/nix/nix.conf this way"
    conf = textwrap.dedent(found.group(1)) + "\n"
    assert f"!include {GUEST}/nix.conf\n" in conf
    return conf


def effective(tmp_path: Path, snapshot: str | None) -> tuple[int, dict[str, str], str]:
    """Nix's settings with the guest's nix.conf; `snapshot` is the included file (None: not there)."""
    conf = tmp_path / "conf"
    conf.mkdir()
    included = tmp_path / "snapshot" / "nix.conf"
    if snapshot is not None:
        included.parent.mkdir()
        included.write_text(snapshot)
    (conf / "nix.conf").write_text(guest_conf().replace(f"{GUEST}/nix.conf", str(included)))
    env = {k: v for k, v in os.environ.items() if not k.startswith("NIX_")}
    env.update(NIX_CONF_DIR=str(conf), NIX_USER_CONF_FILES=str(conf / "user.conf"), HOME=str(tmp_path))
    values = {}
    for name in ("substituters", "trusted-public-keys", "trusted-users", "netrc-file"):
        got = subprocess.run(["nix", "config", "show", name], env=env, text=True, capture_output=True)
        if got.returncode != 0:
            return got.returncode, values, got.stderr
        values[name] = got.stdout.strip()
    return 0, values, ""


@pytest.mark.parametrize("snapshot", [None, ""], ids=["snapshot-not-mounted", "no-caches"])
def test_without_configured_caches_the_guest_uses_only_the_pinned_defaults(tmp_path, snapshot):
    """A missing snapshot (the mount failed) and an empty one both leave the default caches and keys."""
    code, values, error = effective(tmp_path, snapshot)
    assert code == 0, error
    assert values["substituters"].split() == DEFAULTS
    assert [key.split(":")[0] for key in values["trusted-public-keys"].split()] == ["cache.nixos.org-1", "tsugumi.cachix.org-1"]
    assert values["trusted-users"].split() == ["root"]
    assert GUEST not in values["netrc-file"]


def test_a_configured_cache_is_added_to_the_defaults(tmp_path):
    """The snapshot adds its cache and key; the defaults stay and root remains the only trusted user."""
    snapshot = (f"extra-substituters = https://cache.example.org/team\n"
                f"extra-trusted-public-keys = {KEY}\n"
                f"netrc-file = {GUEST}/netrc\n")
    code, values, error = effective(tmp_path, snapshot)
    assert code == 0, error
    assert values["substituters"].split() == DEFAULTS + ["https://cache.example.org/team"]
    assert values["trusted-public-keys"].split()[-1] == KEY
    assert len(values["trusted-public-keys"].split()) == 3
    assert values["trusted-users"].split() == ["root"]
    assert values["netrc-file"] == f"{GUEST}/netrc"


def test_a_snapshot_nix_cannot_parse_is_detected_before_the_daemon_starts(tmp_path):
    """/init asks `nix config show` first: a broken snapshot fails there, and is then left out."""
    code, _, error = effective(tmp_path, "extra-substituters https://cache.example.org/team\n")
    assert code != 0 and "nix.conf" in error
    init = (ROOT / "macOS/Container/vm-boot.nix").read_text()
    check = init.index("nix config show substituters >/dev/null")
    assert init.index("mount -t virtiofs -o ro tsugumi-binary-caches") < check < init.index("nix-daemon --daemon")
    assert "umount /run/tsugumi/binary-caches" in init[check:init.index("nix-daemon --daemon")]
