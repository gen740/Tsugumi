"""Dependency gates belong to the final physical component boundaries."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("boundaries", ROOT / "tests/tools/check_boundaries.py")
boundaries = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boundaries)


def test_swift_packages_respect_owned_boundaries():
    assert not boundaries.check_swift()
