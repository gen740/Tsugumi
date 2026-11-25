"""Exercise session and route isolation through the native Card and project RPC."""

import json
import uuid

import pytest

from harness.daemon import write_project
from harness.fixtures import mock_plan
from harness.native_card import native_card
from harness.scenario import Project, run
from harness.views import (EMPTY_OUTPUTS, MESSAGE_MAX, NATIVE_VIEW_SETTINGS, ZERO_ROUTE,
                           declaration, display, open_view, return_credit)
from projects import wait_run, wait_run_notification
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC import ViewSendRequest
from Tsugumi.Trace.Client import Client
from tsugumi_client import RpcFailure, text

RESIDENT = r"""
#include <pthread.h>
#include <string.h>

static const tsugumi_effects *effects;
static void *host;
static tsugumi_view_session left, right;
static tsugumi_route opener, right_route, dead, extra;

static void *serve(void *unused) {
    (void)unused;
    tsugumi_view_event event;
    while (effects->recv(host, &event, 20000) == 0) {
        if (event.kind == 1) {
            if (event.data.size != 0 || memcmp(&event.session, &left, sizeof(left)))
                break;
            tsugumi_route unused_route;
            const uint8_t dropped = 42;
            if (effects->send(host, &left, &dropped, 1) ||
                effects->route_register(host, &left, &unused_route) != TSUGUMI_VIEW_WINDOW_CLOSED)
                break;
            effects->send(host, &right, event.route.bytes, sizeof(event.route.bytes));
            continue;
        }
        if (event.data.size == 1 && event.data.data[0] == 1) {
            if (effects->route_register(host, &left, &extra)) break;
            effects->send(host, &right, extra.bytes, sizeof(extra.bytes));
        } else if (event.data.size == 1 && event.data.data[0] == 2) {
            if (effects->route_unregister(host, &dead)) break;
            effects->send(host, &right, dead.bytes, sizeof(dead.bytes));
        } else if (event.data.size == 1 && event.data.data[0] == 3) {
            if (effects->view_close(host, &left) || effects->view_close(host, &left))
                break;
        } else {
            uint8_t response[17];
            memcpy(response, event.route.bytes, 16);
            response[16] = event.data.size == 0 ? 0 : event.data.data[0];
            effects->send(host, &right, response, sizeof(response));
        }
    }
    return NULL;
}

int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    effects = call->effects;
    host = call->host;
    if (effects->view_open(host, "missing", 0, 0, 0, 0, &opener, &left) >= 0 ||
        effects->view_open(host, "page", 3, 0, 0, 0, &opener, &left) >= 0 ||
        effects->view_open(host, "page", 0, 0, 800, 600, &opener, &left) >= 0 ||
        effects->view_open(host, "page", 1, 0, 800, 0, &opener, &left) >= 0 ||
        effects->view_open(host, "page", 1, 4, 800, 600, &opener, &left) >= 0 ||
        effects->view_open(host, "page", 1,
                           TSUGUMI_VIEW_HIDE_CURSOR | TSUGUMI_VIEW_LOCK_POINTER,
                           800, 600, &opener, &left) ||
        effects->view_open(host, "page", 2, 0, 0, 0, &right_route, &right) ||
        effects->route_register(host, &left, &dead))
        return 1;
    static const uint8_t oversized[TSUGUMI_MESSAGE_MAX + 1];
    if (effects->send(host, &left, oversized, sizeof(oversized)) >= 0 ||
        effects->send(host, &left, opener.bytes, sizeof(opener.bytes)) ||
        effects->send(host, &right, right_route.bytes, sizeof(right_route.bytes)))
        return 2;
    pthread_t thread;
    if (pthread_create(&thread, NULL, serve, NULL)) return 3;
    pthread_detach(thread);
    return write_result(call, result, result_bytes, sizeof(result_bytes));
}
"""


def setup(core_factory, project, tmp_path, source, *, reuse=False):
    store = tmp_path / "store"
    store.mkdir()
    blob = store / "page.html"
    blob.write_text("<p>view</p>")
    package = native_card(tmp_path / "package", source)
    daemon = core_factory(mocked_builds=True, env={"TSUGUMI_MOCK_STORE": str(store)})
    ui = daemon.connect(client=Client.Web)
    other = daemon.connect()
    other.open()
    graph = Project(project).card(
        "work", {**NATIVE_VIEW_SETTINGS, "reuseWorker": reuse},
        outputs=EMPTY_OUTPUTS, plan=mock_plan([package], views=[declaration(blob)]))
    graph.save(ui)
    subscription = text(ui.open().subscriptionId)
    return daemon, ui, other, subscription


class ByteViewSend(ViewSendRequest.ViewSendRequestT):
    def Pack(self, builder):
        session = builder.CreateString(self.session)
        route = builder.CreateString(self.route)
        payload = builder.CreateByteVector(self.bytes)
        ViewSendRequest.Start(builder)
        ViewSendRequest.AddSession(builder, session)
        ViewSendRequest.AddRoute(builder, route)
        ViewSendRequest.AddBytes(builder, payload)
        return ViewSendRequest.End(builder)


def send(client, session, route=ZERO_ROUTE, payload=b""):
    client.wait_reply(client.send("ViewSendRequest", ByteViewSend(session, route, payload)))


def route_from(data):
    return str(uuid.UUID(bytes=bytes(data)))


def test_headless_view_has_routes_and_drops_sends(project, tmp_path, core_factory):
    store = tmp_path / "store"
    store.mkdir()
    blob = store / "page.html"
    blob.write_text("<p>headless</p>")
    package = native_card(tmp_path / "headless", r"""
#include <string.h>

int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    const tsugumi_effects *effects = call->effects;
    void *host = call->host;
    tsugumi_route opener, second;
    tsugumi_view_session session;
    if (effects->view_open(host, "page", TSUGUMI_VIEW_PANEL, 0, 0, 0,
                           &opener, &session) ||
        effects->route_register(host, &session, &second) ||
        memcmp(&opener, &second, sizeof(opener)) == 0)
        return 1;
    static const uint8_t payload[TSUGUMI_MESSAGE_MAX];
    for (int i = 0; i < 5; ++i)
        if (effects->send(host, &session, payload, sizeof(payload))) return 2;
    tsugumi_view_event event;
    if (effects->recv(host, &event, 50) != 1 ||
        effects->route_unregister(host, &second))
        return 3;
    return write_result(call, result, result_bytes, sizeof(result_bytes));
}
""")
    daemon = core_factory(mocked_builds=True, env={"TSUGUMI_MOCK_STORE": str(store)})
    client = daemon.connect()
    Project(project).card("work", NATIVE_VIEW_SETTINGS, outputs=EMPTY_OUTPUTS,
                          plan=mock_plan([package], views=[declaration(blob)])).save(client)
    client.open()
    assert wait_run(client, run(client)).state == RunState.succeeded
    assert not any(note.valueType in {NotificationPayload.ViewOpened,
                                      NotificationPayload.DisplayUpdated}
                   for note in client.notifications.copy())


def denied(client, name, **fields):
    with pytest.raises(RpcFailure) as exc:
        client.call(name, **fields)
    assert exc.value.kind == "permission_denied"


def test_routes_are_private_to_the_window_and_close_once_after_return(project, tmp_path, core_factory):
    _, ui, other, subscription = setup(core_factory, project, tmp_path, RESIDENT, reuse=True)
    run_id = run(ui)
    first_view_event = ui.wait_notification(
        lambda note: note.valueType in {NotificationPayload.ViewOpened, NotificationPayload.DisplayUpdated},
        timeout=15)
    assert first_view_event.valueType == NotificationPayload.ViewOpened
    left_note, right_note = first_view_event.value, open_view(ui)
    left, right = text(left_note.session), text(right_note.session)
    assert left != right
    assert left_note.card.segments == [b"work"]
    assert (text(left_note.name), text(left_note.type)) == ("page", "html")
    assert (left_note.mode, left_note.width, left_note.height) == (1, 800, 600)
    assert left_note.flags == 3
    assert (right_note.mode, right_note.width, right_note.height) == (2, 0, 0)
    assert right_note.flags == 0
    first = display(ui, left)
    opener = route_from(first.bytes)
    other_route = route_from(display(ui, right).bytes)
    assert len({left, right, opener, other_route, ZERO_ROUTE}) == 5
    assert wait_run(ui, run_id).state == RunState.succeeded
    assert wait_run_notification(other, run_id, timeout=15).state == RunState.succeeded
    assert not any(n.valueType in {NotificationPayload.ViewOpened, NotificationPayload.DisplayUpdated}
                   for n in other.notifications.copy())

    denied(other, "ViewSendRequest", session=left, route=ZERO_ROUTE, bytes=b"x")
    denied(other, "ViewCloseRequest", session=left)
    denied(other, "ViewCreditRequest", subscriptionId=subscription, session=left,
           bytes=16, creditId=first.creditId)
    with pytest.raises(RpcFailure) as exc:
        send(ui, left, other_route, b"x")
    assert exc.value.kind == "invalid_request"
    with pytest.raises(RpcFailure) as exc:
        send(ui, left, payload=b"\0" * (MESSAGE_MAX + 1))
    assert exc.value.kind == "invalid_request"

    send(ui, left, payload=b"\1")
    extra = route_from(display(ui, right).bytes)
    send(ui, left, opener, b"\2")
    dead = route_from(display(ui, right).bytes)
    assert len({opener, other_route, extra, dead}) == 4
    with pytest.raises(RpcFailure) as exc:
        send(ui, right, dead, b"x")
    assert exc.value.kind == "invalid_request"
    send(ui, left, dead, b"x")  # Dead, but associated with this window.
    send(ui, left, extra)
    assert bytes(display(ui, right).bytes) == uuid.UUID(extra).bytes + b"\0"

    send(ui, right, payload=b"\3")  # The opener closes from its idle thread.
    ui.wait_notification(lambda note: note.valueType == NotificationPayload.ViewClosed
                         and text(note.value.session) == left, timeout=15)
    seen = {route_from(display(ui, right).bytes) for _ in range(2)}
    assert seen == {opener, extra}
    send(ui, right, payload=b"\xff")
    assert bytes(display(ui, right).bytes) == uuid.UUID(other_route).bytes + b"\xff"
    assert not any(
        (note.valueType == NotificationPayload.ViewClosed and text(note.value.session) == left)
        or (note.valueType == NotificationPayload.DisplayUpdated and text(note.value.display.session) == left)
        for note in ui.notifications.copy())
    ui.call("ViewCloseRequest", session=right)


def test_display_waits_for_real_byte_credit_without_stalling_other_session(project, tmp_path, core_factory):
    gate = tmp_path / "release"
    source = "#define RELEASE " + json.dumps(str(gate)) + r"""
#include <pthread.h>
#include <unistd.h>

static const tsugumi_effects *effects;
static void *host;
static tsugumi_view_session slow, fast;
static uint8_t bytes[TSUGUMI_MESSAGE_MAX];

static void *blast(void *unused) {
    (void)unused;
    for (uint8_t i = 1; i <= 12; ++i) {
        bytes[0] = i;
        if (effects->send(host, &slow, bytes, sizeof(bytes))) return NULL;
    }
    effects->send(host, &fast, (const uint8_t *)"drained", 7);
    return NULL;
}

int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    effects = call->effects;
    host = call->host;
    tsugumi_route route;
    if (effects->view_open(host, "page", 0, 0, 0, 0, &route, &slow) ||
        effects->view_open(host, "page", 0, 0, 0, 0, &route, &fast))
        return 1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, blast, NULL)) return 2;
    while (access(RELEASE, F_OK)) usleep(1000);
    effects->send(host, &fast, (const uint8_t *)"ready", 5);
    pthread_join(thread, NULL);
    return write_result(call, result, result_bytes, sizeof(result_bytes));
}
"""
    _, ui, _, subscription = setup(core_factory, project, tmp_path, source)
    run_id = run(ui)
    slow, fast = (text(open_view(ui).session) for _ in range(2))
    first, second = display(ui, slow), display(ui, slow)
    assert len(first.bytes) == len(second.bytes) == MESSAGE_MAX
    gate.touch()
    assert bytes(display(ui, fast).bytes) == b"ready"
    ui.call("ViewCreditRequest", subscriptionId=subscription, session=fast,
            bytes=len(first.bytes), creditId=first.creditId)
    ui.call("ViewCreditRequest", subscriptionId=subscription, session=slow,
            bytes=1, creditId=first.creditId)
    with pytest.raises(TimeoutError):
        ui.wait_notification(
            lambda note: note.valueType == NotificationPayload.DisplayUpdated
            and text(note.value.display.session) == slow, timeout=0.2)
    assert first.bytes[0] == 1 and second.bytes[0] == 2
    return_credit(ui, subscription, first)
    return_credit(ui, subscription, first)  # A credit ID can only be returned once.
    return_credit(ui, subscription, second)
    for expected in range(3, 13):
        update = display(ui, slow)
        assert len(update.bytes) == MESSAGE_MAX and update.bytes[0] == expected
        return_credit(ui, subscription, update)
    assert bytes(display(ui, fast).bytes) == b"drained"
    assert wait_run(ui, run_id).state == RunState.succeeded


def test_another_process_can_take_over_a_window_after_its_opener_exits(
        project, tmp_path, core_factory):
    store = tmp_path / "store"
    store.mkdir()
    blob = store / "page.html"
    blob.write_text("<p>handoff</p>")
    capability = "#define CAPABILITY " + json.dumps(str(tmp_path / "session")) + "\n"
    opener = native_card(tmp_path / "opener", capability + r"""
#include <stdio.h>

int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    tsugumi_route route;
    tsugumi_view_session session;
    if (call->effects->view_open(call->host, "page", 0, 0, 0, 0, &route, &session))
        return 1;
    FILE *file = fopen(CAPABILITY, "wb");
    if (!file || fwrite(&session, sizeof(session), 1, file) != 1 || fclose(file))
        return 2;
    return write_result(call, result, result_bytes, sizeof(result_bytes));
}
""")
    successor_source = capability + r"""
#include <stdio.h>
#include <string.h>

int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    tsugumi_view_session session;
    FILE *file = fopen(CAPABILITY, "rb");
    if (!file || fread(&session, sizeof(session), 1, file) != 1 || fclose(file))
        return 1;
    tsugumi_route route;
    const int registered = call->effects->route_register(call->host, &session, &route);
#ifdef FOREIGN_PROJECT
    if (registered >= 0 || call->effects->send(call->host, &session, NULL, 0))
        return 2;
#else
    if (registered == TSUGUMI_VIEW_WINDOW_CLOSED)
        return write_result(call, result, result_bytes, sizeof(result_bytes));
    if (registered)
        return 2;
    if (call->effects->send(call->host, &session, route.bytes, sizeof(route.bytes)))
        return 3;
    tsugumi_view_event event;
    if (call->effects->recv(call->host, &event, 10000) || event.kind != 0 ||
        memcmp(&event.route, &route, sizeof(route)) ||
        memcmp(&event.session, &session, sizeof(session)) ||
        event.data.size != 1 || event.data.data[0] != 0xfe)
        return 4;
    if (call->effects->view_close(call->host, &session))
        return 5;
    if (call->effects->recv(call->host, &event, 10000) || event.kind != 1 ||
        memcmp(&event.route, &route, sizeof(route)))
        return 6;
#endif
    return write_result(call, result, result_bytes, sizeof(result_bytes));
}
"""
    successor = native_card(tmp_path / "successor", successor_source)
    remote_copy = native_card(tmp_path / "foreign-successor", "#define FOREIGN_PROJECT\n" + successor_source)
    env = {"TSUGUMI_MOCK_STORE": str(store)}
    ui = core_factory(mocked_builds=True, env=env).connect(client=Client.Web)
    graph = Project(project)
    settings = NATIVE_VIEW_SETTINGS
    graph.card("work", settings, outputs=EMPTY_OUTPUTS,
               plan=mock_plan([opener], views=[declaration(blob)]))
    graph.card("next", settings, outputs=EMPTY_OUTPUTS, plan=mock_plan([successor])).save(ui)
    ui.open()

    first = run(ui)
    session = text(open_view(ui).session)
    assert wait_run(ui, first).state == RunState.succeeded
    send(ui, session, payload=b"\xff")  # The default route died with the opener.

    foreign_project = tmp_path / "foreign"
    write_project(foreign_project)
    foreign_ui = core_factory(project_root=foreign_project, mocked_builds=True, env=env).connect(client=Client.Web)
    Project(foreign_project).card("work", settings, outputs=EMPTY_OUTPUTS,
                                  plan=mock_plan([remote_copy])).save(foreign_ui)
    foreign_ui.open()
    assert wait_run(foreign_ui, run(foreign_ui)).state == RunState.succeeded

    second = run(ui, "next")
    route = route_from(display(ui, session).bytes)
    send(ui, session, route, b"\xfe")
    ui.wait_notification(lambda note: note.valueType == NotificationPayload.ViewClosed
                         and text(note.value.session) == session, timeout=15)
    assert wait_run(ui, second).state == RunState.succeeded
    # A subsequent Card can treat a closed session as a clean handoff end.
    assert wait_run(ui, run(ui, "next")).state == RunState.succeeded
