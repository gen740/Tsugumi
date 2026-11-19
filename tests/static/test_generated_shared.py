"""Files generated from hostd/agent/Shared are current, so the Mac app ships the same prompts and tools."""

import importlib.util
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def test_generated_copies_of_the_shared_files_are_current():
    r = subprocess.run([sys.executable, str(ROOT / "hostd/agent/Shared/generate.py"), "--check"])
    assert r.returncode == 0, "run python3 hostd/agent/Shared/generate.py"


def test_generated_card_templates_are_current():
    for name in ("visualization", "marimo"):
        spec = importlib.util.spec_from_file_location(name, ROOT / f"ui/shared/templates/{name}.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for path, value in module.outputs().items():
            assert path.read_text() == value, f"{path.relative_to(ROOT)} is stale: run python3 ui/shared/templates/{name}.py"
