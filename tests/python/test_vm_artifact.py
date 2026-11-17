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
sources = module("vm_sources", "vm-source-bundle.py")
inventory = module("vm_inventory", "vm-inventory.py")


def fixture(directory):
    with tarfile.open(directory / artifact.PAYLOAD, "w:gz") as tar:
        for name, data in [("vmlinux", b"kernel"), ("rootfs.erofs", b"image"), ("vm-version", b"definition\n")]:
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            tar.addfile(entry, io.BytesIO(data))
    for name in ["vm-licenses.tar.gz", "vm-sources.tar.gz"]:
        (directory / name).write_bytes(b"inventory/sources")
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
    del value["files"]["vm-sources.tar.gz"]
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


def test_sources_include_every_local_input_fetched_input_and_derivation():
    graph = {"core.drv": {"inputs": {"srcs": ["native-fileset", "hosq", "nixpkgs-patch"]},
                           "outputs": {"out": {"path": "core-output"}}},
             "fetch.drv": {"inputs": {"srcs": []},
                           "outputs": {"out": {"path": "upstream-source", "hash": "hash"}}},
             "tool.drv": {"inputs": {"srcs": ["tool-configuration"]},
                          "outputs": {"out": {"path": "build-only-tool"}}}}
    assert set(sources.source_paths(graph)) == {"/nix/store/" + p for p in [
        "core.drv", "fetch.drv", "tool.drv", "native-fileset", "hosq", "nixpkgs-patch", "upstream-source", "tool-configuration"]}


def test_source_split_preserves_bytes_and_only_splits_over_limit(tmp_path):
    archive = tmp_path / "vm-sources.tar.gz"
    data = b"0123456789abcdef"
    archive.write_bytes(data)
    assert sources.split_archive(archive, limit=20, chunk_size=4) == [archive]
    chunks = sources.split_archive(archive, limit=10, chunk_size=4)
    assert b"".join(p.read_bytes() for p in chunks) == data
    assert not archive.exists()


def test_inventory_rejects_unowned_runtime_paths():
    with pytest.raises(KeyError):
        inventory.group_packages({}, ["/nix/store/missing"])
