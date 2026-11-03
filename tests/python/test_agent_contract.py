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
