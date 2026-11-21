"""Executable SDK examples run through the public core RPC."""

from __future__ import annotations

import json
import os
import re
import shutil
import struct
import subprocess

import pytest
from conftest import ROOT, Daemon
from harness.observations import decode_scalar, read_output, read_pages
from projects import run_and_wait
from Tsugumi.Data.DisplayValue import DisplayValue
from Tsugumi.Data.RunState import RunState
from tsugumi_client import new_id, path, text

DOCS = ROOT / "docs"


# ---- CardGuide.md, run as written ----


def guide_files() -> dict[str, dict[str, str]]:
    """The page's files: {variant: {path: content}}; "" is the Python Card."""
    page = (DOCS / "CardGuide.md").read_text()
    files: dict[str, dict[str, str]] = {}
    for m in re.finditer(r"^`([^`\n]+)`(?: \(([^)\n]+)\))?\n\n```\w*\n(.*?)^```", page, flags=re.MULTILINE | re.DOTALL):
        files.setdefault(m.group(2) or "", {})[m.group(1)] = m.group(3)
    return files


@pytest.mark.parametrize("variant", ["", "Python objects", "bash", "C++", "R", "Julia", "Ruby"])
def test_card_guide_runs_as_written(tmp_path, variant):
    files = guide_files()
    root = tmp_path / "guide"
    for name, content in files[""].items():
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
    (root / "layout.json").write_text("{}")
    if variant:
        shutil.rmtree(root / "definitions/multiply/src")
        for name, content in files[variant].items():
            p = root / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
    # A host tsugumid runs code Cards without the sandbox, as conftest.demo_environment does.
    card_file = root / "definitions/multiply/_card.json"
    card_file.write_text(json.dumps({**json.loads(card_file.read_text()), "sandbox": False, "recordIO": True}))
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, status = run_and_wait(c, "start")
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        result = read_output(c, run_id, card="multiply", port="result")
        assert decode_scalar(result, "uint64") == 42
    finally:
        d.cleanup()


@pytest.mark.parametrize("wasm", [False, True], ids=["javascript", "wasm-c"])
def test_card_owned_webgpu_module(tmp_path, wasm):
    """A fetched Card view is executable and its identity changes with its definition."""
    root = tmp_path / "point-cloud"
    fixture = "view-point-cloud-wasm" if wasm else "view-point-cloud"
    shutil.copytree(ROOT / "tests/fixtures" / fixture, root)
    card_file = root / "definitions/cloud/_card.json"
    card_file.write_text(json.dumps({**json.loads(card_file.read_text()), "sandbox": False}))
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, status = run_and_wait(c, "start")
        assert status.state == RunState.succeeded
        display = c.call("RunRecordRequest", runId=run_id, card=path("cloud")).record.display.value
        identity = text(display.module)
        count = json.loads(text(display.configJson))["count"]
        blobs = {text(blob.name): bytes(blob.data) for blob in display.blobs}
        points = list(struct.iter_unpack("<fff", blobs["points"]))
        assert len(points) == count
        code = read_pages(c, "ModuleReadRequest", module=identity, page_bytes=65536 if wasm else 113)
        node = shutil.which("node")
        if node:
            module = tmp_path / "point-cloud.mjs"
            module.write_bytes(code)
            bundle_variable = "TSUGUMI_WASM_BUNDLE" if wasm else "TSUGUMI_JS_BUNDLE"
            subprocess.run([node, str(ROOT / "ui/web/tests/wasmView.mjs"), str(ROOT)],
                           env={**os.environ, bundle_variable: str(module)}, check=True)
        if wasm:
            definition = root / "definitions/cloud/default.nix"
            definition.write_text(definition.read_text().replace("point-cloud-wasm-view", "second-point-cloud-view"))
            c.call("ProjectReloadRequest", baseRev=c.call("ProjectInspectRequest").rev,
                   opId=new_id(), origin="test")
            second_id, second = run_and_wait(c, "start")
            assert second.state == RunState.succeeded
            second_view = c.call("RunRecordRequest", runId=second_id, card=path("cloud")).record.display.value
            second_identity = text(second_view.module)
            assert second_identity != identity

    finally:
        d.cleanup()


def test_line_chart_uses_binary_view_input(tmp_path):
    """The unchanged SDK call publishes double precision blobs through the module RPC."""
    from tsugumi_client import line_chart_series
    root = tmp_path / "line-chart"
    shutil.copytree(ROOT / "tests/fixtures/view-point-cloud", root)
    definition = root / "definitions/cloud"
    (definition / "_card.json").write_text('{"type":"code","language":"python","inputs":[{"name":"start"}],"sandbox":false}')
    (definition / "default.nix").write_text(
        '{ pkgs, inputs, params }: let sdk = inputs.tsugumi.sdk { languages = [ "python" ]; }; '
        'in { run = inputs.tsugumi.pythonCard { inherit sdk; src = ./src; }; }')
    (definition / "src/main.py").write_text('''def main(inputs, report):
    report.line_chart([
        {"id": "raw", "label": "Timestamp", "x": [1e12, 1e12 + 0.125], "y": [1e12, 1e12 + 0.25]},
        {"id": "mean", "label": "Mean", "x": [1e12, 1e12 + 0.125], "y": [1e12 + 0.125, 1e12 + 0.125]},
    ])
''')
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        rid, status = run_and_wait(c, "start")
        assert status.state == RunState.succeeded
        display = c.call("RunRecordRequest", runId=rid, card=path("cloud")).record.display
        assert display.valueType == DisplayValue.DisplayModule
        series = line_chart_series(display.value)
        assert series[0].x == (1e12, 1e12 + 0.125)
        assert series[0].y == (1e12, 1e12 + 0.25)
        assert series[1].label == "Mean"
        assert series[1].x == (1e12, 1e12 + 0.125)
        assert series[1].y == (1e12 + 0.125, 1e12 + 0.125)
    finally:
        d.cleanup()
