import pytest

from harness.daemon import write_project
from harness.nix_path import test_lock_entries


@pytest.fixture
def project(tmp_path):
    root = tmp_path / "project"
    write_project(root, locked=test_lock_entries())
    return root
