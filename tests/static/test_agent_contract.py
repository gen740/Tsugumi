"""Shared UI assets stay synchronized; Agent behavior is tested over core RPC."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_delegate_schema_matches_the_core_contract():
    shared = json.loads((ROOT / "agent/Shared/agent.json").read_text())
    fixture = json.loads((ROOT / "tests/fixtures/agent-contract.json").read_text())
    assert shared["delegateTool"]["inputSchema"] == fixture["delegateSchema"]
    assert "helper" not in shared


def test_swift_copy_of_the_shared_files_is_current():
    r = subprocess.run([sys.executable, str(ROOT / "agent/Shared/generate.py"), "--check"])
    assert r.returncode == 0, "run python3 agent/Shared/generate.py"


def test_the_english_guide_script_is_the_japanese_one_translated():
    """agent/Shared/guide-script.en.json has the same tours, steps, choices,
    targets and buttons as guide-script.json; only its words differ, and they
    are all English."""
    import json
    import re
    jp = re.compile(r"[\u3040-\u30ff\u3400-\u9fff]")
    ja = json.loads((ROOT / "agent/Shared/guide-script.json").read_text())
    en = json.loads((ROOT / "agent/Shared/guide-script.en.json").read_text())
    problems = []

    def same(a, b, where):
        if isinstance(a, dict) and isinstance(b, dict) and len(a) == len(b):
            if where.endswith(".next") or where.endswith(".keywords"):
                # Choices are keyed by their (translated) words: the same targets, in the same order.
                for (ka, va), (kb, vb) in zip(a.items(), b.items()):
                    same(ka, kb, f"{where}[{ka}]")
                    same(va, vb, f"{where}.{ka}")
            else:
                for k in a:
                    if k not in b:
                        problems.append(f"{where}.{k}: missing")
                    elif k != "_comment":
                        same(a[k], b[k], f"{where}.{k}")
        elif isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
            for i, (x, y) in enumerate(zip(a, b)):
                same(x, y, f"{where}[{i}]")
        elif isinstance(a, str) and isinstance(b, str) and jp.search(a):
            if not b.strip() or jp.search(b):
                problems.append(f"{where}: {b!r}")
        elif a != b:
            problems.append(f"{where}: {a!r} != {b!r}")

    same(ja, en, "")
    assert not problems, problems


def test_draft_contract_has_portless_ends_and_atomic_explicit_rebinding():
    prompt = (ROOT / "agent/Shared/system-prompt.md").read_text()
    request = json.loads((ROOT / "agent/Shared/agent.json").read_text())["draftRequest"]["text"]
    schema = (ROOT / "protocol/tsugumi.fbs").read_text()
    assert '{"card": "<id>", "port": ""}' in prompt
    for text in [prompt, request, schema]:
        assert "SetProject" in text
        assert "incoming" in text and "outgoing" in text
        assert "same" in text
        assert "single" in text or "only one" in text or "exactly one" in text
    assert "provisional ports" not in request


def test_draft_demos_use_only_empty_draft_ends_in_both_languages():
    for lang in ["ja", "en"]:
        for project in (ROOT / "demos" / lang).glob("*-draft/tsugumi.json"):
            drafts = set()
            for file in project.parent.glob("definitions/*/_card.json"):
                definition = json.loads(file.read_text())
                if definition.get("template") == "draft":
                    drafts.add(file.parent.name)
                    assert "inputs" not in definition, file
            for wire in json.loads(project.read_text())["canvas"]["connections"]:
                for side in ["from", "to"]:
                    if wire[side]["card"] in drafts:
                        assert wire[side]["port"] == "", (project, wire)


def test_file_output_instructions_do_not_require_a_card_permission():
    """Published instructions use declared File/Directory ports and an output path."""
    for name in ("agent/Shared/system-prompt.md", "docs/CardGuide.md"):
        source = " ".join((ROOT / name).read_text().split())
        assert "filesystem" not in source, name
        assert 'tsugumi: "file"' in source, name
        assert 'tsugumi: "directory"' in source, name
        assert "run.output_directory" in source, name
