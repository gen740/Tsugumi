"""Shared public-RPC support for core and remaining integration suites."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent / "core"))


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "static: reads only files of the repository (or runs a generator with --check); needs no daemon, "
        "no Nix build of the project and no network. Everything under tests/static is marked so. CI runs "
        "tests/static first, once, in the Static checks job; the heavy jobs run -m 'not static'",
    )


def pytest_collection_modifyitems(items):
    for item in items:
        if "tests/static/" in str(item.path):
            item.add_marker(pytest.mark.static)
