"""Collect traffic through hostd from an authenticated project core."""

import json
from contextlib import closing

import pytest
from harness.host import Host
from tsugumi_client import RpcFailure
from Tsugumi.Trace.CoreTraceRequest import CoreTraceRequestT
from Tsugumi.Trace.Mode import Mode


def test_runtime_mode_switch_and_collection(daemon, tmp_path):
    project_id = daemon.ready["project_id"]
    with closing(Host(tmp_path / "host")) as host, daemon.connect() as core:
        host.call("RegisterProjectRequest", projectId=project_id, endpoint=f"unix:{daemon.unix}")
        host.call("HostActivityRequest")
        assert not host.call("HostTraceRequest", source="").snapshot.entries

        catalog = host.call("HostTraceRequest", mode=Mode.Trace)
        assert catalog.snapshot.mode == Mode.Trace
        assert [(r.id.decode(), r.available) for r in catalog.recorders] == [(project_id, True)]
        host.call("HostActivityRequest")
        core.call("ProjectActivityRequest")
        host_entries = host.call("HostTraceRequest", source="").snapshot.entries
        core_entries = host.call("HostTraceRequest", source=project_id).snapshot.entries
        assert any(e.messageType == b"HostActivityRequest" for e in host_entries)
        assert any(e.messageType == b"ProjectActivityRequest" for e in core_entries)
        assert all(e.contentJson is None for e in host_entries + core_entries)

        catalog = host.call("HostTraceRequest", mode=Mode.Capture)
        assert catalog.snapshot.mode == Mode.Capture
        assert catalog.recorders[0].available
        core.call("ProjectActivityRequest")
        snapshot = host.call("HostTraceRequest", source=project_id).snapshot
        assert snapshot.mode == Mode.Capture
        entries = snapshot.entries
        decoded = [json.loads(e.contentJson) for e in entries if e.contentJson]
        assert any(e["payload"]["value_type"] == "ProjectActivityRequest" for e in decoded), (
            [(e.messageType, e.contentOmission) for e in entries], decoded
        )

        host.call("HostTraceRequest", mode=Mode.Off)
        snapshot = host.call("HostTraceRequest", source=project_id).snapshot
        assert snapshot.mode == Mode.Off
        assert all(e.contentJson is None for e in snapshot.entries)


def test_browser_project_token_cannot_enable_or_read_trace(daemon):
    with daemon.connect(websocket=True) as browser:
        with pytest.raises(RpcFailure, match="permission_denied"):
            request = CoreTraceRequestT()
            request.mode = Mode.Capture
            browser.wait_reply(browser.send("CoreTraceRequest", request))
        with pytest.raises(RpcFailure, match="permission_denied"):
            browser.wait_reply(browser.send("CoreTraceRequest", CoreTraceRequestT()))
