"""File-copy documentation follows streamed saves rather than deleted frame limits."""
from pathlib import Path
import pytest

pytestmark = pytest.mark.static
ROOT = Path(__file__).resolve().parents[2]

@pytest.mark.parametrize("name", ["features", "security"])
def test_copy_documents_describe_streaming_and_private_helper_scratch(name):
    text = (ROOT / "docs" / f"{name}.md").read_text()
    paragraph = next(p for p in text.split("\n\n") if "project_copy" in p and "source manifest" in p)
    assert "12 MiB" not in paragraph
    assert "1..1000" not in paragraph
    assert "private durable staging files" in text
    assert "scratch is not shared" in text
