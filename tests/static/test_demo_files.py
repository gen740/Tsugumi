"""Demo discovery metadata and parity for locally translated executable Cards."""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]

DEMOS = json.loads((ROOT / "demos" / "ja" / "index.json").read_text())["demos"]
DEMO_PROJECTS = sorted((ROOT / "demos").rglob("tsugumi.json"))
NAME = re.compile(r"[a-z][a-z0-9_]{0,63}")
PROJECT_FIELDS = [
    "formatVersion", "name", "nixPath", "trackedPaths", "agentExclude",
    "recordLogs", "recordIO", "logLimitBytes", "outputLimitBytes",
    "outputLimitEntries", "canvas",
]
BASE_FIELDS = {"type", "symbol"}
EXECUTABLE_FIELDS = BASE_FIELDS | {
    "inputs", "maxRuns", "reuseWorker", "timeoutSeconds", "buildTimeoutSeconds",
    "recordLogs", "recordIO", "sandbox", "grants", "systems", "workerCapabilities",
}
CARD_FIELDS = {
    "code": EXECUTABLE_FIELDS,
    "merge": BASE_FIELDS | {"inputs", "maxRuns", "recordIO"},
    "collect": BASE_FIELDS | {"maxRuns", "recordIO"},
    "draft": BASE_FIELDS,
    "trigger": BASE_FIELDS,
    "subgraph": BASE_FIELDS,
}


def test_every_implemented_demo_has_a_field():
    """The guide's first questions list the demos by field (demos/index.json
    "field"): each implemented demo is in one; a Draft is listed apart."""
    bad = [d["id"] for d in DEMOS if bool(d.get("field", "").strip()) == d["id"].endswith("-draft")]
    assert not bad, bad


@pytest.mark.parametrize("demo", ["csharp-calibration", "signal-sweep"])
def test_localized_demos_share_executable_definitions(demo):
    japanese = ROOT / "demos" / "ja" / demo
    english = ROOT / "demos" / "en" / demo
    docs = [json.loads((root / "tsugumi.json").read_text()) for root in (japanese, english)]
    for doc in docs:
        del doc["name"]
    assert docs[0] == docs[1]
    for file in japanese.joinpath("definitions").rglob("*"):
        if not file.is_file() or file.name in {"README.md", "_comments.json"}:
            continue
        relative = file.relative_to(japanese)
        assert file.read_bytes() == (english / relative).read_bytes(), relative


@pytest.mark.parametrize("language", ["ja", "en"])
def test_demo_locks_and_parameters(language):
    for manifest in (ROOT / "demos" / language).rglob("tsugumi.json"):
        declarations = {entry["name"]: entry["url"] for entry in json.loads(manifest.read_text())["nixPath"]}
        lock = json.loads(manifest.with_name("tsugumi.lock").read_text())
        entries = lock["entries"]
        assert [entry["name"] for entry in entries] == sorted(declarations), manifest
        assert {entry["name"]: entry["url"] for entry in entries} == declarations, manifest
        assert all(entry["narHash"].startswith("sha256-") for entry in entries), manifest

        for definition in manifest.parent.glob("definitions/*/_card.json"):
            # A Card without params has no params.nix (a missing file is {}).
            params = definition.with_name("params.nix")
            if params.is_file():
                text = re.sub(r"#[^\n]*", "", params.read_text())
                assert re.sub(r"\s", "", text) != "{}", params


def check_front_matter(path):
    fence, front, _ = path.read_text().split("---\n", 2)
    assert not fence, path
    lines = front.splitlines()
    fields = dict(line.split(": ", 1) for line in lines)
    assert len(lines) == 2 and fields.keys() == {"title", "description"}, path
    assert fields["title"].strip(), path


def check_card(definition):
    path = definition / "_card.json"
    card = json.loads(path.read_text())
    required = CARD_FIELDS[card["type"]]
    optional = {"secrets"} if card["type"] == "code" else set()
    assert required <= card.keys() <= required | optional, path
    assert isinstance(card["symbol"], str) and card["symbol"], path
    for port in card.get("inputs", []):
        assert port.keys() == {"name"} and NAME.fullmatch(port["name"]), path
    if card["type"] == "merge":
        assert card["inputs"], path
    for setting in {"recordLogs", "recordIO"} & card.keys():
        assert card[setting] in {"project", "on", "off"}, (path, setting)
    if card["type"] == "code":
        sandbox = card["sandbox"]
        assert sandbox.keys() == {"filesystem", "pid", "ipc", "identity", "limits"}, path
        assert all(type(sandbox[key]) is bool for key in sandbox.keys() - {"limits"}), path
        assert sandbox["limits"].keys() == {"cpu", "memoryMiB"}, path
        assert 0 <= sandbox["limits"]["cpu"] <= 256, path
        assert 0 <= sandbox["limits"]["memoryMiB"] <= 1048576, path
        grants = card["grants"]
        assert {"network", "gui"} <= grants.keys() <= {"network", "gui", "hostPaths"}, path
        assert type(grants["network"]) is type(grants["gui"]) is bool, path
        for folder in grants.get("hostPaths", []):
            assert folder.keys() == {"path", "name", "write"} and type(folder["write"]) is bool, path
        bits = card["workerCapabilities"]
        assert all(type(bit) is int and 0 <= bit < 64 for bit in bits), path
        assert len(set(bits)) == len(bits), path
        systems = card["systems"]
        assert systems and len(set(systems)) == len(systems), path
        assert set(systems) <= {"aarch64-linux", "x86_64-linux", "aarch64-darwin", "x86_64-darwin"}, path

    check_front_matter(definition / "README.md")
    schema = definition / "_outputs.fbs"
    if schema.exists():
        source = re.sub(r"//[^\n]*|/\*.*?\*/", "", schema.read_text(), flags=re.S)
        outputs = re.search(r"\btable\s+Outputs\s*\{([^}]*)\}", source)
        assert outputs, schema
        assert all(NAME.fullmatch(port) for port in re.findall(r"\b(\w+)\s*:", outputs[1])), schema
    return card["type"]


@pytest.mark.parametrize("manifest", DEMO_PROJECTS, ids=lambda path: str(path.relative_to(ROOT)))
def test_demo_project_files_are_explicit(manifest):
    doc = json.loads(manifest.read_text())
    child = manifest.parent.parent.name == "externals"
    optional = ["boundary"] if child and "boundary" in doc else []
    assert list(doc) == PROJECT_FIELDS + optional
    assert doc["formatVersion"] == 1
    assert all(isinstance(doc[key], list) for key in ("nixPath", "trackedPaths", "agentExclude"))
    assert all(type(doc[key]) is bool for key in ("recordLogs", "recordIO"))
    assert all(type(doc[key]) is int for key in ("logLimitBytes", "outputLimitBytes", "outputLimitEntries"))
    assert doc["canvas"].keys() == {"cards", "connections"}

    cards = doc["canvas"]["cards"]
    assert all(card.keys() == {"id"} and NAME.fullmatch(card["id"]) for card in cards)
    by_id = {card["id"] for card in cards}
    assert len(by_id) == len(cards)
    root = manifest.parent
    folders = {path.name: path for path in (root / "definitions").glob("*") if path.is_dir()}
    assert folders.keys() == by_id
    kinds = {cid: check_card(path) for cid, path in folders.items()}
    children = {cid for cid, kind in kinds.items() if kind == "subgraph"}
    assert {path.name for path in (root / "externals").glob("*") if path.is_dir()} == children
    for cid in children:
        assert (root / "externals" / cid / "tsugumi.json").is_file()

    for wire in doc["canvas"]["connections"]:
        assert wire.keys() == {"from", "to"}
        for item in wire.values():
            assert item.get("card") in by_id
            if kinds[item["card"]] == "draft":
                assert item.keys() == {"card"}
            else:
                assert item.keys() == {"card", "port"} and NAME.fullmatch(item["port"])
    if "boundary" in doc:
        boundary = doc["boundary"]
        assert boundary.keys() == {"inputs", "outputs"}
        for ports in boundary.values():
            assert isinstance(ports, list)
            for port in ports:
                assert port.keys() == {"name", "card", "port"}
                assert port["card"] in by_id and kinds[port["card"]] != "draft"
                assert NAME.fullmatch(port["name"]) and NAME.fullmatch(port["port"])
