"""Executable SDK examples run through the public core RPC."""

from __future__ import annotations

import json
import re
import shutil

import pytest
from conftest import ROOT, Daemon
from projects import run_and_wait
from Tsugumi.Data.RunState import RunState
from tsugumi_client import path, text

DOCS = ROOT / "docs"


# ---- CardGuide.md, run as written ----


def guide_files() -> dict[str, dict[str, str]]:
    """The page's files: {variant: {path: content}}; "" is the Python Card."""
    page = (DOCS / "CardGuide.md").read_text()
    files: dict[str, dict[str, str]] = {}
    for m in re.finditer(r"^`([^`\n]+)`(?: \(([^)\n]+)\))?\n\n```\w*\n(.*?)^```", page, flags=re.MULTILINE | re.DOTALL):
        files.setdefault(m.group(2) or "", {})[m.group(1)] = m.group(3)
    return files


@pytest.mark.parametrize("variant, shown", [("", "21 × 2 = 42"), ("Python objects", "21 × 2 = 42"), ("bash", None),
                                            ("C++", "21 × 2 = 42"), ("R", "21 × 2 = 42"), ("Julia", "21 × 2 = 42"), ("Ruby", "21 × 2 = 42")])
def test_card_guide_runs_as_written(tmp_path, variant, shown):
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
    card_file.write_text(json.dumps({**json.loads(card_file.read_text()), "sandbox": False}))
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
        if shown:
            record = c.call("RunRecordRequest", runId=run_id, card=path("multiply"))
            assert text(record.record.display.value.text) == shown
    finally:
        d.cleanup()


@pytest.mark.parametrize("wasm", [False, True], ids=["javascript", "wasm-c"])
def test_card_owned_webgpu_module(tmp_path, wasm):
    """The view is in the built closure, fetched separately and fixed across data updates."""
    root = tmp_path / "point-cloud"
    fixture = "view-point-cloud-wasm" if wasm else "view-point-cloud"
    shutil.copytree(ROOT / "tests/fixtures" / fixture, root)
    card_file = root / "definitions/cloud/_card.json"
    card_file.write_text(json.dumps({**json.loads(card_file.read_text()), "sandbox": False}))
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.succeeded
        display = c.call("RunRecordRequest", runId=run_id, card=path("cloud")).record.display.value
        identity = text(display.module)
        suffix = "-point-cloud-wasm-view" if wasm else "-point-cloud-view"
        assert identity.startswith("/nix/store/") and identity.endswith(suffix)
        assert json.loads(text(display.configJson)) == {"count": 12000}
        assert text(display.blobs[0].name) == "points"
        assert len(display.blobs[0].data) == 12000 * 12
        # The module was already built and retained by the Card manifest's closure.
        code = bytearray()
        offset = 0
        while True:
            page = c.call("ModuleReadRequest", module=identity, offset=offset, limitBytes=65536 if wasm else 113)
            assert page.offset == offset and text(page.name) == "index.js"
            code.extend(page.bytes)
            if page.eof:
                break
            assert page.nextOffset > offset
            offset = page.nextOffset
        assert b"requestAdapter" in code and b"createRenderPipeline" in code
        if wasm:
            import base64
            from pathlib import Path

            output = Path(identity)
            assert (output / "index.js").is_symlink()
            template = (output / "index.js").resolve()
            assert template.parent.name.endswith("-tsugumi-wasm-view-template")
            # The Card has only its Wasm and a reference, never a template copy.
            binary = (output / "view.wasm").read_bytes()
            assert binary.startswith(b"\0asm\1\0\0\0")
            assert base64.b64encode(binary) in code
            assert code.endswith(template.read_bytes())
            assert int((output / "size-bytes").read_text()) == len(binary)
            assert int((template.parent / "size-bytes").read_text()) == template.stat().st_size
            assert len(binary) < 32768
        else:
            assert len(code) < 10000
        again_id, again = run_and_wait(c, "start", timeout=1800)
        assert again.state == RunState.succeeded
        again_display = c.call("RunRecordRequest", runId=again_id, card=path("cloud")).record.display.value
        assert text(again_display.module) == identity
    finally:
        d.cleanup()


def test_line_chart_uses_binary_view_input(tmp_path):
    """The unchanged SDK call publishes double precision blobs through the module RPC."""
    import struct
    from tsugumi_client import line_chart_series
    root = tmp_path / "line-chart"
    shutil.copytree(ROOT / "tests/fixtures/view-point-cloud", root)
    definition = root / "definitions/cloud"
    (definition / "_card.json").write_text('{"language":"python","inputs":[{"name":"start"}],"sandbox":false}')
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
        rid, status = run_and_wait(c, "start", timeout=1800)
        assert status.state == RunState.succeeded
        display = c.call("RunRecordRequest", runId=rid, card=path("cloud")).record.display
        assert display.valueType == 7
        series = line_chart_series(display.value)
        assert series[0].x == (1e12, 1e12 + 0.125)
        assert series[0].y == (1e12, 1e12 + 0.25)
        assert series[1].label == "Mean"
        config = json.loads(text(display.value.configJson))
        assert config["series"][1]["offset"] == 2
        assert "x" not in config["series"][0] and "y" not in config["series"][0]
        blobs = {text(b.name): bytes(b.data) for b in display.value.blobs}
        assert blobs["x"] == struct.pack("<dddd", 1e12, 1e12 + 0.125, 1e12, 1e12 + 0.125)
        page = c.call("ModuleReadRequest", module=text(display.value.module), limitBytes=1048576)
        assert page.eof and b"createRenderPipeline" in bytes(page.bytes)
    finally:
        d.cleanup()
