"""Definition identity, redistribution completeness and full source-closure selection."""
import hashlib
import importlib.util
import io
import json
import tarfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "vm/guest/tools" / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


artifact = module("vm_artifact", "vm-artifact.py")
inventory = module("vm_inventory", "vm-inventory.py")


def fixture(directory):
    with tarfile.open(directory / artifact.PAYLOAD, "w:gz") as tar:
        for name, data in [("vmlinux", b"kernel"), ("rootfs.erofs", b"image"), ("vm-version", b"definition\n")]:
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            tar.addfile(entry, io.BytesIO(data))
    (directory / "vm-licenses.tar.gz").write_bytes(b"inventory")
    value = {"identity": "definition", "system": "aarch64-linux", "revision": "commit",
             "files": {p.name: artifact.digest(p) for p in directory.iterdir()},
             "members": {name: hashlib.sha256(data).hexdigest() for name, data in
                         [("vmlinux", b"kernel"), ("rootfs.erofs", b"image"), ("vm-version", b"definition\n")]}}
    (directory / "vm-manifest.json").write_text(json.dumps(value))
    return value


def test_exact_identity_revision_and_checksums(tmp_path):
    fixture(tmp_path)
    artifact.verify(tmp_path, "definition", "commit")
    with pytest.raises(ValueError, match="definition mismatch"):
        artifact.verify(tmp_path, "older-definition")
    with pytest.raises(ValueError, match="revision mismatch"):
        artifact.verify(tmp_path, "definition", "different-commit")
    (tmp_path / artifact.PAYLOAD).write_bytes(b"truncated")
    with pytest.raises(ValueError, match="checksum mismatch"):
        artifact.verify(tmp_path, "definition")


def test_missing_redistribution_assets_fail(tmp_path):
    value = fixture(tmp_path)
    del value["files"]["vm-licenses.tar.gz"]
    (tmp_path / "vm-manifest.json").write_text(json.dumps(value))
    with pytest.raises(ValueError, match="incomplete"):
        artifact.verify(tmp_path, "definition")


def test_wrong_archive_stamp_fails_even_with_updated_archive_checksum(tmp_path):
    value = fixture(tmp_path)
    value["identity"] = "another-definition"
    (tmp_path / "vm-manifest.json").write_text(json.dumps(value))
    with pytest.raises(ValueError, match="payload identity"):
        artifact.verify(tmp_path, "another-definition")


def test_attachment_checksums_match_extracted_bytes(tmp_path):
    value = fixture(tmp_path)
    value["members"]["rootfs.erofs"] = "0" * 64
    (tmp_path / "vm-manifest.json").write_text(json.dumps(value))
    with pytest.raises(ValueError, match="attachment checksum"):
        artifact.verify(tmp_path, "definition")


def test_an_artifact_carries_no_source_archive(tmp_path):
    value = fixture(tmp_path)
    (tmp_path / "vm-sources.tar.gz").write_bytes(b"sources")
    value["files"]["vm-sources.tar.gz"] = artifact.digest(tmp_path / "vm-sources.tar.gz")
    (tmp_path / "vm-manifest.json").write_text(json.dumps(value))
    with pytest.raises(ValueError, match="incomplete"):
        artifact.verify(tmp_path, "definition")


def test_tsugumi_outputs_are_one_fixed_entry_and_others_are_not():
    for name in ["tsugumid", "tsugumi-vm-system", "tsugumi-supervisor", "tsugumi-vm-init", "passwd", "nix.conf"]:
        assert inventory.is_own(name)
    for name in ["nix", "bash-interactive", "glibc", "hosq-source", "flatbuffers"]:
        assert not inventory.is_own(name)


def test_inventory_rejects_unowned_runtime_paths():
    with pytest.raises(KeyError):
        inventory.group_packages({}, ["/nix/store/missing"])


def test_inventory_names_fixed_outputs_that_derivation_json_leaves_out(monkeypatch):
    graph = {"a-source.drv": {"name": "source", "outputs": {"out": {"hash": "h", "method": "nar"}}},
             "b-core.drv": {"name": "core", "outputs": {"out": {"path": "b-core"}}}}
    monkeypatch.setattr(inventory, "run", lambda *cmd: "/nix/store/" + "0" * 32 + "-source\n")
    inventory.add_fixed_output_paths(graph)
    assert graph["a-source.drv"]["outputs"]["out"]["path"] == "0" * 32 + "-source"
    monkeypatch.setattr(inventory, "run", lambda *cmd: "/nix/store/" + "0" * 32 + "-other\n")
    graph["a-source.drv"]["outputs"]["out"].pop("path")
    with pytest.raises(SystemExit):
        inventory.add_fixed_output_paths(graph)
