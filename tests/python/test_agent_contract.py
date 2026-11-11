"""The Swift and the web Agent share agent/Shared/ (system prompt, tools,
texts): the Swift copy is generated from it, and the web half of the
contract (tests/fixtures/agent-*.json) runs here; the Swift half is
macOS/CoreTests (ContractTests)."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "web"


def test_swift_copy_of_the_shared_files_is_current():
    r = subprocess.run([sys.executable, str(ROOT / "agent/Shared/generate.py"), "--check"])
    assert r.returncode == 0, "run python3 agent/Shared/generate.py"


@pytest.mark.skipif(not shutil.which("node") or not (WEB / "node_modules").is_dir()
                    or not (WEB / "src/gen/tsugumi.bfbs").exists(),
                    reason="needs node, web/node_modules and web/src/gen (npm ci; npm run gen)")
def test_web_agent_keeps_the_contract():
    r = subprocess.run(["node", "scripts/contract.mjs"], cwd=WEB, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr


@pytest.mark.skipif(not shutil.which("node") or not (WEB / "node_modules").is_dir()
                    or not (WEB / "src/gen/tsugumi.bfbs").exists(),
                    reason="needs node, web/node_modules and web/src/gen (npm ci; npm run gen)")
def test_web_server_signs_in_with_chatgpt_and_uses_the_plan():
    """Sign in with ChatGPT in the web server against the model fixture (also
    the OAuth issuer): sign-in, the 0600 record, the plan's request rules,
    refreshes, the usage limit and the sign-out (web/server/chatgptCheck.ts)."""
    r = subprocess.run(["node", "scripts/contract.mjs", "chatgpt"], cwd=WEB, capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr


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
