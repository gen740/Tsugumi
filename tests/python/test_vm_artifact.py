"""Guest artifact assembly and integrity."""
import argparse
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


def fixture(directory):
    with tarfile.open(directory / artifact.PAYLOAD, "w:gz") as tar:
        for name, data in [("vmlinux", b"kernel"), ("rootfs.erofs", b"image"), ("vm-version", b"definition\n")]:
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            tar.addfile(entry, io.BytesIO(data))
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


def test_build_emits_payload_manifest_and_checksums(tmp_path):
    image, out = tmp_path / "image", tmp_path / "artifact"
    image.mkdir()
    fixture(image)
    artifact.build(argparse.Namespace(image=image, directory=out, revision="commit"))
    assert {path.name for path in out.iterdir()} == {
        artifact.PAYLOAD, "vm-manifest.json", "SHA256SUMS"}
    value = artifact.verify(out, "definition", "commit")
    assert value["files"] == {artifact.PAYLOAD: artifact.digest(out / artifact.PAYLOAD)}
    assert (out / artifact.PAYLOAD).read_bytes() == (image / artifact.PAYLOAD).read_bytes()
    assert (out / "SHA256SUMS").read_text() == "".join(
        f"{artifact.digest(out / name)}  {name}\n"
        for name in sorted([artifact.PAYLOAD, "vm-manifest.json"]))
