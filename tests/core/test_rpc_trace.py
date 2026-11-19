"""Collect traffic through hostd from an authenticated project core."""

import json
from contextlib import closing

import pytest
from harness.host import Host, HostFailure
from tsugumi_client import RpcFailure
from Tsugumi.Trace.CoreTraceRequest import CoreTraceRequestT
from Tsugumi.Trace.Mode import Mode


@pytest.fixture
def debug_core(core_factory):
    return core_factory(env={"TSUGUMI_RPC_TRACE": "1"})


def test_runtime_mode_switch_and_collection(debug_core, tmp_path):
    daemon = debug_core
    project_id = daemon.ready["project_id"]
    with closing(Host(tmp_path / "host", rpc_trace=True)) as host, daemon.connect() as core:
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


def test_host_refuses_trace_without_debug_launch(tmp_path):
    with closing(Host(tmp_path / "host")) as host:
        for fields in ({"mode": Mode.Capture}, {"source": ""}):
            with pytest.raises(HostFailure) as failure:
                host.call("HostTraceRequest", **fields)
            assert failure.value.code == "permission_denied"
        assert not host.call("HostActivityRequest").busy


def test_core_refuses_trace_without_debug_launch(core_factory):
    core = core_factory(env={"TSUGUMI_RPC_TRACE": "0"})
    with core.connect() as client:
        for mode in (Mode.Capture, None):
            request = CoreTraceRequestT()
            request.mode = mode
            with pytest.raises(RpcFailure) as failure:
                client.wait_reply(client.send("CoreTraceRequest", request))
            assert failure.value.kind == "permission_denied"


def test_browser_project_token_cannot_enable_or_read_trace(debug_core):
    with debug_core.connect(websocket=True) as browser:
        with pytest.raises(RpcFailure, match="permission_denied"):
            request = CoreTraceRequestT()
            request.mode = Mode.Capture
            browser.wait_reply(browser.send("CoreTraceRequest", request))
        with pytest.raises(RpcFailure, match="permission_denied"):
            browser.wait_reply(browser.send("CoreTraceRequest", CoreTraceRequestT()))
