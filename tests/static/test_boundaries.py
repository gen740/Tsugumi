"""Dependency gates belong to the final physical component boundaries."""
import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("boundaries", ROOT / "tests/tools/check_boundaries.py")
boundaries = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boundaries)


def test_production_includes_respect_owned_boundaries():
    assert not boundaries.check()


def test_relative_and_public_header_probes_cannot_cross_private_boundaries():
    for owner, forbidden in [
        ("rpc", "protocol/host.fbs"), ("util", "rpc/include/tsugumi/rpc/transport.hh"),
        ("host", "tsugumid/runtime/src/version_record.hh"),
        ("worker", "tsugumid/runtime/src/service.hh"),
        ("guest", "tsugumid/runtime/src/nix_eval.hh"),
        ("document", "tsugumid/runtime_support/include/tsugumi/runtime_support/cache_usage.hh"),
    ]:
        assert not boundaries.can_include(owner, ROOT / forbidden)
    assert boundaries.can_include("rpc", ROOT / "util/include/tsugumi/util/fs.hh")
