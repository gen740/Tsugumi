"""Guest artifact assembly and integrity."""
import argparse
import importlib.util
import io
import json
import shutil
import subprocess
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
             "files": {p.name: artifact.digest(p) for p in directory.iterdir()}}
    (directory / "vm-manifest.json").write_text(json.dumps(value))
    return value


def test_exact_identity_revision_and_checksums(tmp_path):
    image, out = tmp_path / "image", tmp_path / "artifact"
    image.mkdir()
    fixture(image)
    artifact.build(argparse.Namespace(image=image, directory=out, revision="commit"))
    checker = ["sha256sum"] if shutil.which("sha256sum") else ["shasum", "-a", "256"]
    subprocess.run([*checker, "-c", "SHA256SUMS"], cwd=out, check=True)
    artifact.verify(out, "definition", "commit")
    with pytest.raises(ValueError):
        artifact.verify(out, "older-definition")
    with pytest.raises(ValueError):
        artifact.verify(out, "definition", "different-commit")
    (out / artifact.PAYLOAD).write_bytes(b"truncated")
    with pytest.raises(ValueError):
        artifact.verify(out, "definition")


def test_wrong_archive_stamp_fails_even_with_updated_archive_checksum(tmp_path):
    value = fixture(tmp_path)
    value["identity"] = "another-definition"
    (tmp_path / "vm-manifest.json").write_text(json.dumps(value))
    with pytest.raises(ValueError):
        artifact.verify(tmp_path, "another-definition")
