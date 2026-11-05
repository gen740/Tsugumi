"""The documents say what is true: their links and paths exist, every test
file, demo and document is listed, Protocol.md and the static site are what
their generators make, the tests Evidence.md names exist, and the Card written
out in CardGuide.md runs (in Python, bash, C++, R and Julia), taken from the page itself."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from conftest import ROOT, Daemon
from projects import run_and_wait
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState

DOCS = ROOT / "docs"
PAGES = [ROOT / "README.md", ROOT / "AGENTS.md", ROOT / "INSTALL.md", ROOT / "THIRD_PARTY_NOTICES.md", *sorted(p for p in DOCS.glob("*.md") if p.name != "index.md")]

# Paths made by a build or at run time, and files of an earlier setup named as history.
NOT_IN_GIT = {"web/src/gen", "web/dist", "web/node_modules", "agent/guide/", "tests/python/stress_shutdown.py"}


def strip_code(s: str) -> str:
    return re.sub(r"```.*?```", "", s, flags=re.S)


def slug(heading: str) -> str:
    """GitHub's anchor of a heading."""
    h = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading.strip())
    h = re.sub(r"[`*]", "", h).lower()
    h = re.sub(r"[^\w\- 　]", "", h)
    return h.replace(" ", "-")


def anchors(p: Path) -> set[str]:
    return {slug(h) for h in re.findall(r"^#+\s+(.*)$", strip_code(p.read_text()), flags=re.M)}


def test_links_and_anchors_exist():
    bad = []
    for page in PAGES:
        for m in re.finditer(r"\]\(([^)\s]+)\)|href=\"([^\"]+)\"|src=\"([^\"]+)\"", strip_code(page.read_text())):
            link = m.group(1) or m.group(2) or m.group(3)
            if re.match(r"[a-z]+:", link):
                continue
            target, _, anchor = link.partition("#")
            file = (page.parent / target).resolve() if target else page
            if not file.exists():
                bad.append(f"{page.name}: {link}")
            elif anchor and file.suffix == ".md" and anchor not in anchors(file):
                bad.append(f"{page.name}: {link} (no such heading)")
    assert not bad, "\n".join(bad)


def test_paths_named_in_backticks_exist():
    bad = []
    pattern = r"`((?:core|sdk|macOS|agent|web|tests|ui|protocol|demos|plugins|nix|cmake)/[A-Za-z0-9_./\-]+)`"
    for page in PAGES:
        for name in set(re.findall(pattern, page.read_text())):
            if name not in NOT_IN_GIT and not (ROOT / name.rstrip("/")).exists():
                bad.append(f"{page.name}: {name}")
    assert not bad, "\n".join(bad)


def test_every_document_is_in_the_index():
    index = (DOCS / "README.md").read_text()
    missing = [p.name for p in DOCS.glob("*.md") if p.name not in ("README.md", "index.md") and f"({p.name})" not in index]
    assert not missing


def test_every_test_file_is_in_the_test_table():
    table = (DOCS / "Development.md").read_text()
    missing = [p.name for p in (ROOT / "tests/python").glob("test_*.py") if p.name not in table]
    assert not missing


def test_every_demo_is_explained():
    demos = json.loads((ROOT / "demos/index.json").read_text())
    demos = demos if isinstance(demos, list) else demos["demos"]
    explained = (DOCS / "Demos.md").read_text()
    missing = [d["path"] for d in demos if d["path"].split("/")[-1] not in explained]
    assert not missing


def test_protocol_page_is_generated_from_the_schema():
    r = subprocess.run([sys.executable, str(ROOT / "protocol/generate_docs.py"), "--check"])
    assert r.returncode == 0, "run python3 protocol/generate_docs.py"


def test_evidence_names_tests_that_exist():
    evidence = (DOCS / "Evidence.md").read_text()
    named = re.findall(r"`(test_\w+\.py)(?:::(test_\w+))?`", evidence)
    assert named
    missing = []
    for file, test in named:
        source = ROOT / "tests/python" / file
        if not source.exists() or (test and f"def {test}(" not in source.read_text()):
            missing.append(f"{file}::{test}")
    assert not missing, missing


def test_site_is_generated_from_the_documents():
    if not shutil.which("node") or not (ROOT / "web/node_modules/marked").exists():
        pytest.skip("needs node and web/node_modules")
    r = subprocess.run(["node", str(DOCS / "site/build.mjs"), "--check"], capture_output=True, text=True)
    assert r.returncode == 0, "run node docs/site/build.mjs\n" + r.stdout + r.stderr


# ---- CardGuide.md, run as written ----


def guide_files() -> dict[str, dict[str, str]]:
    """The page's files: {variant: {path: content}}; "" is the Python Card."""
    page = (DOCS / "CardGuide.md").read_text()
    files: dict[str, dict[str, str]] = {}
    for m in re.finditer(r"^`([^`\n]+)`(?:（([^）\n]+)）)?\n\n```\w*\n(.*?)^```", page, flags=re.M | re.S):
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


def test_llms_txt_guides_agents_to_every_document():
    site = "https://gen740.github.io/Tsugumi/"
    guide = (DOCS / "llms.txt").read_text()
    assert re.match(r"# Tsugumi\n\n> \S", guide), "an H1, then the summary as a blockquote"
    links = re.findall(r"^- \[[^\]]+\]\((https://[^)\s]+)\)", guide, flags=re.M)
    listed = {u[len(site):] for u in links if u.startswith(site)}
    assert not [u for u in listed if not (DOCS / u).exists()]
    documents = re.findall(r"^\| \[[^\]]+\]\(([^)]+)\) \|", (DOCS / "README.md").read_text(), flags=re.M)
    expected = {f"pages/{Path(d).stem.lower().replace('_', '-')}.md" for d in documents}
    assert not expected - listed
    assert guide.index("## リポジトリ") < guide.index("## Optional"), "Optional comes last: agents may skip it"


def test_markdown_pages_have_links_that_work_on_the_site():
    bad = []
    for page in [DOCS / "index.md", *sorted((DOCS / "pages").glob("*.md"))]:
        for m in re.finditer(r"\]\(([^)\s]+)\)|href=\"([^\"]+)\"|src=\"([^\"]+)\"", strip_code(page.read_text())):
            link = (m.group(1) or m.group(2) or m.group(3)).partition("#")[0]
            if link and not re.match(r"[a-z]+:", link) and not (page.parent / link).exists():
                bad.append(f"{page.name}: {link}")
    assert not bad, bad


def test_pages_point_to_their_markdown_and_the_guide():
    for page in [DOCS / "index.html", *sorted((DOCS / "pages").glob("*.html"))]:
        head = page.read_text().split("</head>", 1)[0]
        markdown = re.search(r'<link rel="alternate" type="text/markdown" href="([^"]+)">', head)
        guide = re.search(r'<link rel="describedby" href="([^"]+)">', head)
        assert markdown and (page.parent / markdown.group(1)).exists(), page.name
        assert guide and (page.parent / guide.group(1)).resolve() == DOCS / "llms.txt", page.name


def test_no_bold_marker_is_left_unparsed():
    """A ** that CommonMark does not close (next to Japanese punctuation) shows as it is, on GitHub too."""
    pages = [DOCS / "index.html", *sorted((DOCS / "pages").glob("*.html"))]
    left = []
    for page in pages:
        body = page.read_text().split("<article", 1)[1].split("</article>", 1)[0]
        body = re.sub(r"<(pre|code)\b.*?</\1>", "", body, flags=re.S)
        if "**" in re.sub(r"<[^>]+>", "", body):
            left.append(page.name)
    assert not left, left
