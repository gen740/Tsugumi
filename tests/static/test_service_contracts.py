"""The service boundary is defined once, in the shared FlatBuffers schemas."""
import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.static
ROOT = Path(__file__).resolve().parents[2]


def test_shared_types_and_conversation_independent_requests_have_one_owner():
    common = (ROOT / "protocol/service_common.fbs").read_text()
    agent = (ROOT / "protocol/agent.fbs").read_text()
    host = (ROOT / "protocol/host.fbs").read_text()
    for name in ("HelloRequest", "HelloResult", "Failure", "Acknowledgement",
                 "ProviderSelection", "Access", "GrantScope", "PermissionKind",
                 "Usage", "TextResult"):
        declaration = rf"(?:table|enum) {name}\b"
        assert len(re.findall(declaration, common)) == 1, name
        assert not re.search(declaration, agent), name
        assert not re.search(declaration, host), name
    for name in ("DemoListRequest", "DemoReadRequest", "VersionNoteRequest"):
        assert re.search(rf"table {name}\b", host)
        assert name not in agent
    for name in ("ConfigureRequest", "ConversationSummarizeRequest", "ToolCallRequest",
                 "AccessRequest", "AccessResolveRequest", "AccessRevokeRequest", "AccessListRequest"):
        assert re.search(rf"table {name}\b", agent)
        assert name not in host
    for schema in (agent, host):
        assert 'include "service_common.fbs";' in schema
    assert not re.search(r"\w+:\[[^]]+\]\s*\([^)]*\brequired\b", host)
    assert 'file_identifier "TSA1"' in agent
    assert 'file_identifier "TSH1"' in host
