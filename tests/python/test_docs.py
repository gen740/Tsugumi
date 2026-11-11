"""Generated site output and executable SDK examples stay usable.

The examples run through the public core RPC; editorial links and document
inventories are not test contracts.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from conftest import ROOT, Daemon
from projects import run_and_wait
from Tsugumi.Data.RunState import RunState
from tsugumi_client import path, text

DOCS = ROOT / "docs"


def test_protocol_page_is_generated_from_the_schema():
    r = subprocess.run([sys.executable, str(ROOT / "protocol/generate_docs.py"), "--check"], check=False)
    assert r.returncode == 0, "run python3 protocol/generate_docs.py"


def test_site_is_generated_from_the_documents():
    modules = Path(os.environ.get("TSUGUMI_WEB_NODE_MODULES", ROOT / "web/node_modules"))
    if not shutil.which("node") or not (modules / "marked").exists():
        pytest.skip("needs node and the Web node_modules")
    r = subprocess.run(["node", str(DOCS / "site/build.mjs"), "--check"], check=False, capture_output=True, text=True)
    assert r.returncode == 0, "run node docs/site/build.mjs\n" + r.stdout + r.stderr


# ---- CardGuide.md, run as written ----


def guide_files() -> dict[str, dict[str, str]]:
    """The page's files: {variant: {path: content}}; "" is the Python Card."""
    page = (DOCS / "CardGuide.md").read_text()
    files: dict[str, dict[str, str]] = {}
    for m in re.finditer(r"^`([^`\n]+)`(?: \(([^)\n]+)\))?\n\n```\w*\n(.*?)^```", page, flags=re.MULTILINE | re.DOTALL):
        files.setdefault(m.group(2) or "", {})[m.group(1)] = m.group(3)
    return files


@pytest.mark.parametrize("variant, shown", [("", "21 × 2 = 42"), ("bash", None), ("C++", "21 × 2 = 42"),
                                            ("R", "21 × 2 = 42"), ("Julia", "21 × 2 = 42")])
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


def test_no_bold_marker_is_left_unparsed():
    """A ** that CommonMark does not close (next to Japanese punctuation) shows as it is, on GitHub too."""
    pages = [DOCS / "index.html", *sorted((DOCS / "pages").glob("*.html"))]
    left = []
    for page in pages:
        body = page.read_text().split("<article", 1)[1].split("</article>", 1)[0]
        body = re.sub(r"<(pre|code)\b.*?</\1>", "", body, flags=re.DOTALL)
        if "**" in re.sub(r"<[^>]+>", "", body):
            left.append(page.name)
    assert not left, left
