"""Shared UI assets stay synchronized; Agent behavior is tested over core RPC."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def structural_schema(value):
    if isinstance(value, dict):
        return {key: structural_schema(item) for key, item in value.items() if key != "description"}
    if isinstance(value, list):
        return [structural_schema(item) for item in value]
    return value


def test_delegate_schema_matches_the_core_contract():
    shared = json.loads((ROOT / "hostd/agent/Shared/agent.json").read_text())
    fixture = json.loads((ROOT / "tests/fixtures/agent-contract.json").read_text())
    assert structural_schema(shared["delegateTool"]["inputSchema"]) == fixture["delegateSchema"]
    assert "helper" not in shared


def test_swift_copy_of_the_shared_files_is_current():
    r = subprocess.run([sys.executable, str(ROOT / "hostd/agent/Shared/generate.py"), "--check"])
    assert r.returncode == 0, "run python3 hostd/agent/Shared/generate.py"


def test_the_english_guide_script_is_the_japanese_one_translated():
    """hostd/agent/Shared/guide-script.en.json has the same tours, steps, choices,
    targets and buttons as guide-script.json; only its words differ, and they
    are all English."""
    import json
    import re
    jp = re.compile(r"[\u3040-\u30ff\u3400-\u9fff]")
    ja = json.loads((ROOT / "hostd/agent/Shared/guide-script.json").read_text())
    en = json.loads((ROOT / "hostd/agent/Shared/guide-script.en.json").read_text())
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


def test_file_copy_schema():
    shared = json.loads((ROOT / "hostd/agent/Shared/agent.json").read_text())
    tools = {tool["name"]: tool for tool in shared["hostTools"]}
    copy = tools["project_copy"]
    assert copy["mutating"]
    assert set(copy["inputSchema"]["properties"]["source"]["enum"]) == {"attached", "card", "demo", "scratch"}
    assert "maxItems" not in copy["inputSchema"]["properties"]["files"]
    assert "bytes" not in copy["inputSchema"]["properties"]
    assert "base_rev" in copy["inputSchema"]["required"]


def test_run_wait_schema_is_shared_with_external_mcp():
    shared = json.loads((ROOT / 'hostd/agent/Shared/agent.json').read_text())
    wait = shared['runWait']
    assert wait['schema']['type'] == 'integer' and wait['schema']['minimum'] == 0
    fixtures = json.loads((ROOT / 'tests/fixtures/agent-tools.json').read_text())
    for tool in fixtures:
        if tool['name'] in ('run', 'run_status'):
            assert structural_schema(tool['inputSchema']['properties']['wait_seconds']) == structural_schema(wait['schema'])


def test_always_sent_prompt_stays_small_and_its_reference_is_bundled():
    prompt = (ROOT / "hostd/agent/Shared/system-prompt.md").read_bytes()
    assert (ROOT / "demos/reference/cards.md").is_file()
