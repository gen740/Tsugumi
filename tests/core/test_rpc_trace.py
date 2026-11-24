"""Collect traffic through hostd from an authenticated project core."""

import json
import select
import socket
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing, contextmanager

import pytest
from harness.daemon import DAEMON, NIX, WORKER
from harness.fixtures import mock_plan
from harness.host import Host, HostConnection, HostFailure
from harness.native_card import native_card
from harness.scenario import Project, run
from harness.views import (EMPTY_OUTPUTS, NATIVE_VIEW_SETTINGS, ZERO_ROUTE, declaration,
                           display, open_view)
from tsugumi_client import Client, RpcFailure, make, new_id, text
from Tsugumi.Host.EnvironmentAction import EnvironmentAction
from Tsugumi.Host.EnvironmentPhase import EnvironmentPhase
from Tsugumi.Host.EventPayload import EventPayload
from Tsugumi.Host.Setting import SettingT
from Tsugumi.Trace.Client import Client as ClientKind
from Tsugumi.Trace.CoreTraceRequest import CoreTraceRequestT
from Tsugumi.Trace.Direction import Direction
from Tsugumi.Trace.Mode import Mode


@pytest.fixture
def debug_core(core_factory):
    return core_factory(env={"TSUGUMI_RPC_TRACE": "1"})


def test_live_trace_follows_host_and_core(debug_core, tmp_path):
    project_id = debug_core.ready["project_id"]
    with closing(Host(tmp_path / "host", rpc_trace=True)) as host, debug_core.connect() as core:
        assert not host.call("HostTraceRequest", source="").snapshot.entries
        assert host.call("HostTraceRequest", mode=Mode.Trace).snapshot.mode == Mode.Trace

        host.call("RegisterProjectRequest", projectId=project_id, endpoint=f"unix:{debug_core.unix}")
        event = host.next_event()
        assert event.valueType == EventPayload.TraceChanged and event.value.catalog
        catalog = host.call("HostTraceRequest")
        assert {r.id.decode() for r in catalog.recorders if r.available} == {project_id}
        host.call("HostTraceRequest", source="")
        host.call("HostTraceRequest", source=project_id)

        host.call("HostActivityRequest")
        core.call("ProjectActivityRequest")
        changed = set()
        # No further RPC is needed for either recorder to wake the viewer.
        while changed != {"", project_id}:
            event = host.next_event()
            assert event.valueType == EventPayload.TraceChanged
            if not event.value.catalog:
                changed.add((event.value.source or b"").decode())

        host_entries = host.call("HostTraceRequest", source="").snapshot.entries
        core_entries = host.call("HostTraceRequest", source=project_id).snapshot.entries
        assert any(e.messageType == b"HostActivityRequest" for e in host_entries)
        assert any(e.messageType == b"ProjectActivityRequest" for e in core_entries)
        assert all(e.contentJson is None for e in host_entries + core_entries)

        catalog = host.call("HostTraceRequest", mode=Mode.Capture)
        assert catalog.snapshot.mode == Mode.Capture
        core.call("ProjectActivityRequest")
        snapshot = host.call("HostTraceRequest", source=project_id).snapshot
        assert snapshot.mode == Mode.Capture
        decoded = [json.loads(e.contentJson) for e in snapshot.entries if e.contentJson]
        assert any(e["payload"]["value_type"] == "ProjectActivityRequest" for e in decoded)

        host.call("HostTraceRequest", mode=Mode.Off)
        snapshot = host.call("HostTraceRequest", source=project_id).snapshot
        assert snapshot.mode == Mode.Off
        assert all(e.contentJson is None for e in snapshot.entries)


def test_trace_traffic_including_failures_is_excluded(debug_core, tmp_path):
    project_id = debug_core.ready["project_id"]
    with closing(Host(tmp_path / "host", rpc_trace=True)) as host, debug_core.connect() as core:
        host.call("RegisterProjectRequest", projectId=project_id, endpoint=f"unix:{debug_core.unix}")
        host.call("HostTraceRequest", mode=Mode.Trace)
        host.call("HostTraceRequest", source="")
        host.call("HostTraceRequest", source=project_id)
        host.call("HostActivityRequest")
        core.call("ProjectActivityRequest")
        changed = set()
        while changed != {"", project_id}:
            notice = host.next_event().value
            changed.add((notice.source or b"").decode())

        with pytest.raises(HostFailure) as failure:
            host.call("HostTraceRequest", source="missing")
        assert failure.value.code == "unavailable"
        with pytest.raises(RpcFailure) as failure:
            core.wait_reply(core.send("CoreTraceRequest", CoreTraceRequestT()))
        assert failure.value.kind == "busy"

        host_entries = host.call("HostTraceRequest", source="").snapshot.entries
        core_entries = host.call("HostTraceRequest", source=project_id).snapshot.entries
        assert {(e.direction, e.messageType) for e in host_entries} == {
            (Direction.Request, b"HostActivityRequest"), (Direction.Reply, b"HostActivity"),
        }
        assert {(e.direction, e.messageType) for e in core_entries} == {
            (Direction.Request, b"ProjectActivityRequest"), (Direction.Reply, b"ProjectActivity"),
        }


def test_live_views_stay_out_of_traces(project, core_factory, tmp_path):
    """View requests, replies and notifications stay out of capture and metadata."""
    marker = b"tsugumi-view-marker-7f3a"
    blob = tmp_path / "store" / "page.html"
    blob.parent.mkdir()
    blob.write_bytes(b"<p>" + marker + b"</p>")
    package = native_card(tmp_path / "package", r"""
int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    tsugumi_route route;
    tsugumi_view_session session;
    if (call->effects->view_open(call->host, "page", 0, 0, 0, 0, &route, &session) ||
        call->effects->send(call->host, &session, (const uint8_t *)"display", 7))
        return 1;
    tsugumi_view_event event;
    if (call->effects->recv(call->host, &event, 10000) || event.kind != 0 ||
        call->effects->recv(call->host, &event, 10000) || event.kind != 1)
        return 2;
    *result = (tsugumi_bytes){result_bytes, sizeof(result_bytes)};
    return 0;
}
""")
    core = core_factory(mocked_builds=True, env={"TSUGUMI_RPC_TRACE": "1",
                                                  "TSUGUMI_MOCK_STORE": str(blob.parent)})
    project_id = core.ready["project_id"]
    with closing(Host(tmp_path / "host", rpc_trace=True)) as host, core.connect(client=ClientKind.Web) as client:
        host.call("RegisterProjectRequest", projectId=project_id, endpoint=f"unix:{core.unix}")
        assert host.call("HostTraceRequest", mode=Mode.Capture).snapshot.mode == Mode.Capture
        Project(project).card("work", NATIVE_VIEW_SETTINGS,
                              outputs=EMPTY_OUTPUTS,
                              plan=mock_plan([package], views=[declaration(blob)])).save(client)
        opened = client.open()
        hidden = []

        def call(name, /, **fields):
            rid = client.send(name, make(name, **fields))
            hidden.append(rid)
            return client.wait_reply(rid)

        run(client)
        session = text(open_view(client).session)
        update = display(client, session)
        assert marker in bytes(call("CardViewReadRequest", session=session).bytes)
        with pytest.raises(RpcFailure):
            call("CardViewReadRequest", session=new_id())
        call("ViewCreditRequest", subscriptionId=text(opened.subscriptionId),
             session=session, bytes=7, creditId=update.creditId)
        call("ViewSendRequest", session=session, route=ZERO_ROUTE, bytes=marker)
        call("ViewCloseRequest", session=session)
        # A later request is captured, so the recorder was on throughout.
        client.call("ProjectActivityRequest")

        entries = host.call("HostTraceRequest", source=project_id).snapshot.entries
        assert any(e.messageType == b"ProjectActivityRequest" and e.contentJson for e in entries)
        assert not [e.messageType for e in entries if e.requestId in hidden]
        assert not {e.messageType for e in entries} & {
            b"ViewCloseRequest", b"ViewSendRequest", b"ViewCreditRequest",
            b"CardViewReadRequest", b"FileContent", b"ViewOpened",
            b"ViewRouteRegistered", b"DisplayUpdated"}
        captured = "".join("".join(e.contentJson.decode().split()) for e in entries if e.contentJson)
        assert marker.decode() not in captured and ",".join(map(str, marker)) not in captured


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
        assert_trace_refused(client)


def test_browser_project_token_cannot_enable_or_read_trace(debug_core):
    with debug_core.connect(websocket=True) as browser:
        with pytest.raises(RpcFailure, match="permission_denied"):
            request = CoreTraceRequestT()
            request.mode = Mode.Capture
            browser.wait_reply(browser.send("CoreTraceRequest", request))
        with pytest.raises(RpcFailure, match="permission_denied"):
            browser.wait_reply(browser.send("CoreTraceRequest", CoreTraceRequestT()))


def test_trace_connection_does_not_duplicate_host_events(tmp_path):
    with closing(Host(tmp_path / "host", rpc_trace=True)) as host, \
            closing(HostConnection(host.ready, trace=True)) as viewer:
        viewer.call("HostTraceRequest", mode=Mode.Trace)
        viewer.call("HostTraceRequest", source="")
        setting = SettingT()
        setting.key = "app.language"
        setting.valueJson = '"en"'
        revision = host.call("SettingsGetRequest", profile="trace-test").revision
        host.call("SettingsUpdateRequest", profile="trace-test", revision=revision,
                  changes=[setting])
        assert host.next_event().valueType == EventPayload.SettingsChanged
        assert viewer.next_event().valueType == EventPayload.TraceChanged
        snapshot = viewer.call("HostTraceRequest", source="").snapshot
        assert sum(e.messageType == b"SettingsChanged" for e in snapshot.entries) == 1
        assert not viewer.events


@pytest.mark.parametrize("rpc_trace", [False, True])
def test_host_launches_native_core_with_its_trace_grant(project, tmp_path, rpc_trace):
    args = ("--tsugumid", str(DAEMON), "--worker", str(WORKER), "--nix-dir", str(NIX),
            "--environment-dir", str(tmp_path / "environments"))
    fields = {"project": str(project)}
    with closing(Host(tmp_path / "host", rpc_trace=rpc_trace, args=args)) as host, \
            closing(HostConnection(host.ready, trace=True)) as viewer:
        env = host.call("EnvironmentRequest", action=EnvironmentAction.Start, **fields)
        try:
            assert env.phase == EnvironmentPhase.Ready, env.message
            with Client(unix=env.unixSocket.decode()) as core:
                core.hello()
                if not rpc_trace:
                    assert_trace_refused(core)
                    return
                project_id = env.projectId.decode()
                reply = viewer.call("HostTraceRequest", mode=Mode.Trace)
                assert [r.id.decode() for r in reply.recorders if r.available] == [project_id]
                snapshot = viewer.call("HostTraceRequest", source=project_id).snapshot
                core.call("ProjectActivityRequest")
                await_change(viewer, source=env.projectId)
                delta = viewer.call("HostTraceRequest", source=project_id, generation=snapshot.generation,
                                    after=max((e.sequence for e in snapshot.entries or []), default=0)).snapshot
                assert any(e.messageType == b"ProjectActivityRequest" for e in delta.entries)
        finally:
            host.call("EnvironmentRequest", action=EnvironmentAction.Stop, **fields)


def await_change(viewer, *, source=None, catalog=False):
    while True:
        event = viewer.next_event()
        assert event.valueType == EventPayload.TraceChanged
        if event.value.catalog == catalog and event.value.source == source:
            return


def assert_trace_refused(client, capability=None):
    for mode in (Mode.Capture, None):
        request = CoreTraceRequestT()
        request.mode = mode
        request.capability = capability
        with pytest.raises(RpcFailure) as failure:
            client.wait_reply(client.send("CoreTraceRequest", request))
        assert failure.value.kind == "permission_denied"


def test_closed_trace_link_reconnects_to_the_same_core(debug_core, tmp_path):
    project_id = debug_core.ready["project_id"]
    with closing(Host(tmp_path / "host", rpc_trace=True)) as host, \
            closing(HostConnection(host.ready, trace=True)) as viewer, \
            debug_core.connect() as core, \
            reconnecting_proxy(debug_core.unix) as (endpoint, sever):
        host.call("RegisterProjectRequest", projectId=project_id, endpoint=f"unix:{endpoint}")
        viewer.call("HostTraceRequest", mode=Mode.Trace)
        initial = viewer.call("HostTraceRequest", source=project_id).snapshot

        sever.sendall(b"x")
        # No project operation or trace query initiates the reconnect.
        await_change(viewer, catalog=True)
        snapshot = viewer.call("HostTraceRequest", source=project_id,
                               generation=initial.generation).snapshot
        assert snapshot.generation != initial.generation
        assert snapshot.mode == Mode.Trace

        core.call("ProjectActivityRequest")
        await_change(viewer, source=project_id.encode())
        delta = viewer.call("HostTraceRequest", source=project_id).snapshot
        assert any(e.messageType == b"ProjectActivityRequest" for e in delta.entries)


@contextmanager
def reconnecting_proxy(upstream_path):
    """Close the first trace link after its upstream reaches EOF."""
    sever, wake = socket.socketpair()
    with socket.socket(socket.AF_UNIX) as listener, sever, wake:
        endpoint = upstream_path + "-trace"
        listener.bind(endpoint)
        listener.listen()

        def relay():
            for _ in range(2):
                readable, _, _ = select.select([listener, wake], [], [])
                if listener not in readable:
                    return
                downstream, _ = listener.accept()
                with downstream, socket.socket(socket.AF_UNIX) as upstream:
                    upstream.connect(upstream_path)
                    peers = {downstream: upstream, upstream: downstream}
                    while True:
                        readable, _, _ = select.select([*peers, wake], [], [])
                        if wake in readable:
                            if not wake.recv(1):
                                return
                            upstream.shutdown(socket.SHUT_WR)
                            peers.pop(downstream)
                        for peer in readable:
                            if peer not in peers:
                                continue
                            data = peer.recv(65536)
                            if not data:
                                break
                            peers[peer].sendall(data)
                        else:
                            continue
                        break

        with ThreadPoolExecutor(max_workers=1) as worker:
            relayed = worker.submit(relay)
            try:
                yield endpoint, sever
            finally:
                sever.shutdown(socket.SHUT_WR)
                relayed.result()
