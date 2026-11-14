"""Every persisted run table has a portable copy strategy."""
import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.static
ROOT = Path(__file__).resolve().parents[2]


def test_every_run_table_is_copied():
    source = (ROOT / 'core/src/history.cc').read_text()
    schema = source.split('std::string run_tables(', 1)[1].split('// Version of', 1)[0]
    tables = set(re.findall(r'R"SQL\((\w+)\(', schema))
    copied = source.split('constexpr Copied kCopied[] = {', 1)[1].split('\n};', 1)[0]
    strategies = set(re.findall(r'\{"(\w+)"', copied))
    # Schemas are deduplicated by hash and selected through the copied values.
    assert '".schemas(hash, root_type, bfbs) SELECT hash, root_type, bfbs FROM "' in source
    assert tables == strategies | {'schemas'}
