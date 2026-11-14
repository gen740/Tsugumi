"""Permanent Card identity is shared by every client and the core."""
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
pytestmark = pytest.mark.static


def test_name_changes_have_no_identity_rename_rpc_or_history_writer():
    protocol = (ROOT / 'protocol/tsugumi.fbs').read_text()
    assert 'table RenameDefinition' not in protocol
    assert 'table CardRename' not in protocol
    assert 'renamed_cards' not in protocol
    assert 'History::rename_card' not in (ROOT / 'core/src/history.cc').read_text()
    assert 'history_->rename_card' not in (ROOT / 'core/src/service.cc').read_text()
