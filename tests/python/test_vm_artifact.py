"""Guest integrity and runtime package notices."""
import gzip
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


def test_runtime_packages_keep_versions_and_group_own_code():
    paths = ["/nix/store/" + "0" * 32 + "-" + name for name in
             ["bash-5.3", "bash-5.3", "bash-5.2", "tsugumid-0.9.0", "hosq-source", "passwd", "unlisted"]]
    assert inventory.runtime_packages(paths) == [
        {"name": "bash", "version": "5.2"}, {"name": "bash", "version": "5.3"},
        {"name": "unlisted", "version": ""}]


def test_notices_share_spdx_texts_and_report_missing_metadata(tmp_path):
    vm, out, texts = [tmp_path / name for name in ["image", "notices", "texts"]]
    vm.mkdir()
    texts.mkdir()
    config = b"CONFIG_IKCONFIG=y\n"
    (vm / "vmlinux").write_bytes(b"kernelIKCFG_ST" + gzip.compress(config) + b"IKCFG_ED")
    for identifier in ["MIT", "GPL-2.0-only", "GCC-exception-2.0"]:
        (texts / f"{identifier}.txt").write_text(identifier)
    packages = [{"name": "a", "version": "1"}, {"name": "a", "version": "1-lib"},
                {"name": "b", "version": "2"}, {"name": "unknown", "version": "3"}]
    inventory.write_notices(vm, "definition", out, "revision", packages,
                            {"a": {"licenses": ["MIT"], "outputs": ["out", "lib"]},
                             "b": {"licenses": ["MIT WITH GCC-exception-2.0"]}, "unknown": {"licenses": []}}, texts, "6.12.115")
    value = json.loads((out / "inventory.json").read_text())
    assert value["vm_version"] == "definition"
    assert value["packages"][2]["licenses"] == []
    assert value["tsugumi"]["licenses"] == ["MIT"]
    assert (out / "licenses/Tsugumi/LICENSE").read_text() == (ROOT / "LICENSE").read_text()
    assert sorted(p.name for p in (out / "licenses").iterdir()) == ["GCC-exception-2.0.txt", "GPL-2.0-only.txt", "MIT.txt", "Tsugumi"]
    assert (out / "kernel-config").read_bytes() == config
    assert "No SPDX license metadata" in (out / "README.md").read_text()
