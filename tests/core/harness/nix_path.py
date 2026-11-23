"""Fixture-only source locks for the evaluator and kit integration tests."""

import json
import os
from pathlib import Path


def test_lock_entries():
    if text := os.environ.get("TSUGUMI_TEST_NIX_LOCK"):
        return json.loads(text)["nixPath"]
    # Source-tree invocations outside the packaged runner use the same pins.
    nodes = json.loads((Path(__file__).resolve().parents[3] / "flake.lock").read_text())["nodes"]
    root = nodes["root"]["inputs"]
    entries = []
    for name in ("nixpkgs", "tsugumi-kit"):
        node = nodes[root[name]]
        original = node["original"]
        url = "{type}:{owner}/{repo}/{ref}".format(**original)
        entries.append({"name": name, "url": url, "locked": node["locked"]})
    return entries


def declarations(entries):
    return [{key: entry[key] for key in ("name", "url")} for entry in entries]


def write_lock(root, entries):
    (root / "tsugumi.lock").write_text(json.dumps({"nixPath": sorted(entries, key=lambda entry: entry["name"])}))
