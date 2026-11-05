"""What the license documents say against what is pinned and packaged. The built
app itself is checked by macOS/tools/check-app-licenses.sh in the release workflow."""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
NOTICES = (ROOT / "THIRD_PARTY_NOTICES.md").read_text()
INVENTORY = ROOT / "third-party" / "vm-boot" / "inventory.json"
# Written in this repository (vm-boot.nix), so without a license of their own.
OWN_PREFIXES = ("tsugumi-",)
OWN = {"group", "hosts", "nix.conf", "nsswitch.conf", "passwd"}


def test_notices_name_every_pinned_swift_package():
    """A package update that forgets the notices fails here."""
    pins = json.loads((ROOT / "macOS" / "Package.resolved").read_text())["pins"]
    missing = [p["identity"] for p in pins if p["state"]["revision"] not in NOTICES and p["state"]["version"] not in NOTICES]
    assert not missing


def test_notices_name_the_pinned_hosq():
    rev = json.loads((ROOT / "flake.lock").read_text())["nodes"]["hosq"]["locked"]["rev"]
    assert rev in NOTICES


def test_the_vm_source_carries_the_licenses():
    entries = (ROOT / "macOS" / "Container" / "vm-source.txt").read_text().split("\n")
    assert "LICENSE" in entries and "THIRD_PARTY_NOTICES.md" in entries


def test_the_app_draws_math_with_the_only_font_it_ships():
    """build-app.sh keeps only SwiftMath's default font: choosing another would fail at run time."""
    sources = (ROOT / "macOS" / "Sources").rglob("*.swift")
    uses = [s.name for s in sources if re.search(r"MTFontManager|MathFont\b|\.font\s*=\s*MT", s.read_text())]
    assert not uses


def test_vm_inventory_has_a_license_for_every_package():
    inventory = json.loads(INVENTORY.read_text())
    assert inventory["vm_version"].endswith("-tsugumi-vm-boot-1")
    licenses = INVENTORY.parent / "licenses"
    for package in inventory["packages"]:
        name = package["name"]
        if name in OWN or name.startswith(OWN_PREFIXES):
            continue
        assert package["licenses"], name
        assert package["sources"], name
        assert any(f.is_file() for f in (licenses / name).rglob("*")), name
    assert (INVENTORY.parent / "kernel-config").read_text().startswith("#\n# Automatically generated file")


@pytest.mark.skipif(not (ROOT / "web" / "dist" / "server.js").exists(), reason="needs web/dist (cd web && npm run build)")
def test_web_dist_has_the_licenses_of_what_it_bundles():
    licenses = ROOT / "web" / "dist" / "licenses"
    assert (licenses / "LICENSE").is_file() and (licenses / "THIRD_PARTY_NOTICES.md").is_file()
    listed = (licenses / "README.md").read_text()
    # Imported by the bundles (react, marked, flatbuffers) and bundled in the viewer's vega.min.js (d3-*).
    for package in ("react", "react-dom", "marked", "flatbuffers", "vega", "d3-array"):
        assert f"| {package} |" in listed, package
        assert any((licenses / package).iterdir()), package
