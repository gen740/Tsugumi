"""Files generated from hostd/agent/Shared are current, so the Mac app ships the same prompts and tools."""

from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def test_generated_copies_of_the_shared_files_are_current():
    r = subprocess.run([sys.executable, str(ROOT / "hostd/agent/Shared/generate.py"), "--check"])
    assert r.returncode == 0, "run python3 hostd/agent/Shared/generate.py"
