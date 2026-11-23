"""Live view declarations and bytes at the public project subscription."""

import json
import socket
import sqlite3
import struct
from pathlib import Path

import pytest

from harness.fixtures import mock_plan
from harness.native_card import native_card
from harness.proxy import ReplyProxy
from harness.scenario import Project, run, wait_run
from tsugumi_client import change, decode_message, encode_request, make, new_id, path, text
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload
from Tsugumi.RPC.Payload import Payload
from Tsugumi.RPC.RunEventValue import RunEventValue
from Tsugumi.Trace.CoreTraceRequest import CoreTraceRequestT
from Tsugumi.Trace.Mode import Mode


OUTPUTS = "table Outputs {} root_type Outputs;"
VIEWS = [
    {"blob": "/nix/store/example-notes", "name": "notes", "type": "html"},
    {"blob": "/nix/store/example-plot", "name": "plot", "type": "dawn"},
]
LIVE_NOTES = {
    NotificationPayload.CardViewsUpdated,
    NotificationPayload.DisplayUpdated,
}


def await_views(client):
    return client.wait_notification(lambda n: n.valueType == NotificationPayload.CardViewsUpdated, timeout=10).value


def view_fields(update):
    return [{field: text(getattr(view, field)) for field in ("name", "type", "blob")} for view in update.views]


def test_views_are_declared_before_a_run_and_replaced_on_definition_change(project, core_factory):
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    graph = Project(project).card("work", {"type": "code"}, outputs=OUTPUTS,
                                  plan=mock_plan(["/unused"], views=VIEWS))
    graph.save(client)
    opened = client.open()
    sub = text(opened.subscriptionId)
    assert opened.snapshot.run.runId is None
    client.call("ViewSetVisibleRequest", subscriptionId=sub, cards=[path("work")])
    update = await_views(client)
    assert update.card.segments == [b"work"]
    assert view_fields(update) == VIEWS

    # A different subscriber gets the current list without having to run the Card.
    other = daemon.connect()
    other.call("ViewSetVisibleRequest", subscriptionId=text(other.open().subscriptionId), cards=[path("work")])
    assert view_fields(await_views(other)) == VIEWS
    graph.card("work", {"type": "code"}, outputs=OUTPUTS,
               plan=mock_plan(["/unused"], views=VIEWS[1:])).save(client)
    assert view_fields(await_views(client)) == VIEWS[1:]
    assert view_fields(await_views(other)) == VIEWS[1:]
    current = client.call("ProjectInspectRequest")
    config = json.loads(text(current.projectJson))
    config["canvas"]["cards"] = []
    client.call("ProjectSaveRequest", baseRev=current.rev, opId=new_id(), origin="test", changes=[
        change("SetProject", json=json.dumps(config)), change("RemoveDefinition", id="work"),
    ])
    assert view_fields(await_views(client)) == []
    assert view_fields(await_views(other)) == []


@pytest.mark.environment
def test_real_nix_publishes_view_files_without_building_the_card(project, core_factory):
    from projects import card, write_nix_project

    definition = {"type": "code", "_outputs": OUTPUTS, "_nix": """
{ inputs, params }: {
  run = throw "the view build must leave run lazy";
  views = [{ name = "notes"; type = "html"; blob = ./notes.html; }];
}
"""}
    write_nix_project(project, {"work": definition}, [card("work", "work")], [],
                      {"definitions/work/notes.html": "<!doctype html><p>Waiting for data</p>"})
    client = core_factory(project_root=project).connect()
    sub = text(client.open().subscriptionId)
    client.call("ViewSetVisibleRequest", subscriptionId=sub, cards=[path("work")])
    update = client.wait_notification(lambda n: n.valueType == NotificationPayload.CardViewsUpdated,
                                      timeout=90).value
    assert not update.failure, update.failure and text(update.failure.diagnostic)
    note, = update.views
    assert text(note.name) == "notes" and text(note.type) == "html"
    blob = Path(text(note.blob))
    assert blob.is_relative_to("/nix/store")
    assert blob.read_text() == "<!doctype html><p>Waiting for data</p>"


CARD = r"""
#include <pthread.h>
#include <unistd.h>

static void *host;
static const tsugumi_effects *effects;
static const uint8_t payload[] = {'l', 'i', 'v', 'e', 0, 0xff, 0x80};

static void *late(void *unused) {
    (void)unused;
    while (access(LATE_GATE, F_OK) != 0) usleep(1000);
    effects->send(host, "unknown|view", payload, sizeof(payload));
    return NULL;
}

int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    static const uint8_t padding[2 << 20];
    host = call->host;
    effects = call->effects;
    if (effects->send(host, "padding", padding, sizeof(padding)) ||
        effects->send(host, "unknown|view", (const uint8_t *)"stale", 5) ||
        effects->send(host, "plot", (const uint8_t *)"different view", 14) ||
        effects->send(host, "unknown|view", payload, sizeof(payload)))
        return 1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, late, NULL) != 0) return 1;
    pthread_detach(thread);
    *result = (tsugumi_bytes){result_bytes, sizeof(result_bytes)};
    return 0;
}
"""


def wire_read(connection):
    frame = ReplyProxy._read_frame(connection)
    assert frame is not None
    return decode_message(frame)


def wire_call(connection, request_id, name, **fields):
    request = encode_request(request_id, name, make(name, **fields))
    connection.sendall(struct.pack("<I", len(request)) + request)
    response = wire_read(connection)
    assert response.payloadType == Payload.Reply and response.requestId == request_id
    return response.payload.value


def test_data_is_fifo_and_post_return_bytes_are_live(project, tmp_path, core_factory):
    gate = tmp_path / "after-run"
    source = "#define LATE_GATE " + json.dumps(str(gate)) + "\n" + CARD
    package = native_card(tmp_path / "view-card", source)
    daemon = core_factory(mocked_builds=True, env={"TSUGUMI_RPC_TRACE": "1"})
    client = daemon.connect()
    client.wait_reply(client.send("CoreTraceRequest", CoreTraceRequestT(mode=Mode.Capture)))
    Project(project).card("work", {"type": "code", "reuseWorker": True, "sandbox": {
        "filesystem": False, "pid": False, "ipc": False, "identity": False,
    }}, outputs=OUTPUTS, plan=mock_plan([package], views=VIEWS)).save(client)

    # A raw subscriber stops reading. The large first payload fills its socket
    # so the small data frames remain queued until the subscriber reads them.
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as slow:
        slow.settimeout(15)
        slow.connect(daemon.unix)
        wire_call(slow, 1, "SessionHelloRequest")
        opened = wire_call(slow, 2, "ProjectOpenRequest")
        wire_call(slow, 3, "ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("work")])
        assert view_fields(wire_read(slow).payload.value) == VIEWS

        run_id = run(client)
        finished = client.wait_notification(
            lambda n: n.valueType == NotificationPayload.RunEvent
            and n.value.valueType == RunEventValue.RunFinished, timeout=15,
        )
        assert finished.value.value.summary.status.state == RunState.succeeded
        assert wait_run(client, run_id).state == RunState.succeeded
        gate.touch()

        events = []
        while len(events) < 5:
            note = wire_read(slow).payload
            if note.valueType == NotificationPayload.DisplayUpdated:
                display = note.value.display
                assert display.card.segments == [b"work"]
                assert display.creditId
                events.append((text(display.view), bytes(display.bytes)))
        opaque = b"live\x00\xff\x80"
        assert [data for view, data in events if view == "unknown|view"] == [b"stale", opaque, opaque]
        assert ("padding", bytes(2 << 20)) in events
        assert ("plot", b"different view") in events

    record = client.call("RunRecordRequest", runId=run_id, card=path("work")).record
    assert record is not None and not hasattr(record, "display")
    trace = client.wait_reply(client.send("CoreTraceRequest", CoreTraceRequestT()))
    assert trace.entries
    assert all(b"Display" not in e.messageType and b"unknown|view" not in (e.contentJson or b"")
               for e in trace.entries)
    client.call("ProjectInspectRequest")
    assert not any(n.valueType in LIVE_NOTES for n in client.notifications.copy())
    daemon.stop()
    history, = project.glob(".tsugumi/**/runs.sqlite")
    with sqlite3.connect(history) as db:
        ddl = [sql for sql, in db.execute("SELECT sql FROM sqlite_master WHERE sql IS NOT NULL")]
    assert all("display" not in sql.lower() for sql in ddl)
    assert opaque not in history.read_bytes()
    restarted = daemon.start().connect()
    restarted.call("ViewSetVisibleRequest", subscriptionId=text(restarted.open().subscriptionId), cards=[path("work")])
    assert view_fields(await_views(restarted)) == VIEWS
    restarted.call("ProjectInspectRequest")
    assert not any(n.valueType == NotificationPayload.DisplayUpdated for n in restarted.notifications.copy())


def test_view_messages_make_a_round_trip_through_project_rpc(project, tmp_path, core_factory):
    from Tsugumi.Trace.Client import Client

    source = r"""
#include <string.h>

int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    const tsugumi_effects *effects = call->effects;
    tsugumi_bytes incoming;
    if (effects->send(call->host, "notes", NULL, 0) != 0) return 1;
    if (effects->recv(call->host, "notes", &incoming, 5000) != 0) return 2;
    if (incoming.size != 4 || memcmp(incoming.data, "\x00\xff\x80v", 4)) return 3;
    if (effects->send(call->host, "notes", incoming.data, incoming.size) != 0) return 4;
    *result = (tsugumi_bytes){result_bytes, sizeof(result_bytes)};
    return 0;
}
"""
    package = native_card(tmp_path / "round-trip-card", source)
    daemon = core_factory(mocked_builds=True)
    ui = daemon.connect(client=Client.Web)
    Project(project).card("work", {"type": "code", "sandbox": {
        "filesystem": False, "pid": False, "ipc": False, "identity": False,
    }}, outputs=OUTPUTS, plan=mock_plan([package], views=VIEWS)).save(ui)
    subscription = text(ui.open().subscriptionId)
    ui.call("ViewSetVisibleRequest", subscriptionId=subscription, cards=[path("work")])
    assert view_fields(await_views(ui)) == VIEWS

    def message():
        return ui.wait_notification(
            lambda note: note.valueType == NotificationPayload.DisplayUpdated, timeout=15,
        ).value.display

    def release(display):
        ui.call("ViewCreditRequest", subscriptionId=subscription, card=display.card,
                view=text(display.view), bytes=len(display.bytes), creditId=display.creditId)

    run_id = run(ui)
    ready = message()
    assert ready.bytes is None or len(ready.bytes) == 0
    # FlatBuffers consumers may decode an empty vector as an absent list.
    ui.call("ViewCreditRequest", subscriptionId=subscription, card=ready.card,
            view="notes", bytes=0, creditId=ready.creditId)
    payload = b"\x00\xff\x80v"
    ui.call("ViewSendRequest", card=path("work"), view="notes", bytes=list(payload))
    echoed = message()
    release(echoed)
    assert bytes(echoed.bytes) == payload
    assert wait_run(ui, run_id).state == RunState.succeeded


# Eight full-size messages fill the four windows while the UI holds credit.
# The ninth waits for a credit. A separate thread drives the slow view.
BURST = r"""
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

static atomic_uint sent;
static atomic_int failed;
static const uint8_t bytes[TSUGUMI_MESSAGE_MAX];

static void *burst(void *arg) {
    const tsugumi_call *call = arg;
    for (unsigned i = 0; i < 9; ++i) {
        if (call->effects->send(call->host, "notes", bytes, sizeof(bytes))) {
            atomic_store(&failed, 1);
            return NULL;
        }
        atomic_fetch_add(&sent, 1);
    }
    return NULL;
}

int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, burst, (void *)call)) return 1;
    while (atomic_load(&sent) < 8 && !atomic_load(&failed)) usleep(1000);
    const uint8_t count = atomic_load(&sent);
    if (call->effects->send(call->host, "plot", &count, 1)) return 2;
    pthread_join(thread, NULL);
    if (atomic_load(&failed)) return 3;
    const uint8_t done = atomic_load(&sent);
    if (call->effects->send(call->host, "plot", &done, 1)) return 4;
    *result = (tsugumi_bytes){result_bytes, sizeof(result_bytes)};
    return 0;
}
"""

UNISOLATED = {"filesystem": False, "pid": False, "ipc": False, "identity": False}


def test_full_view_does_not_block_other_traffic(project, tmp_path, core_factory):
    from Tsugumi.Trace.Client import Client

    slow = native_card(tmp_path / "slow", BURST)
    fast = native_card(tmp_path / "fast", r"""
int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    if (call->effects->send(call->host, "plot", (const uint8_t *)"fast", 4)) return 1;
    *result = (tsugumi_bytes){result_bytes, sizeof(result_bytes)};
    return 0;
}
""")
    client = core_factory(mocked_builds=True).connect(client=Client.Web)
    graph = Project(project).card("start", {"type": "trigger"})
    for name, package in (("slow", slow), ("fast", fast)):
        graph.card(name, {"type": "code", "inputs": [{"name": "trigger"}], "sandbox": UNISOLATED,
                          "reuseWorker": True, "timeoutSeconds": 30},
                   outputs=OUTPUTS, plan=mock_plan([package], views=VIEWS))
        graph.connect("start", "out", name, "trigger")
    graph.save(client)
    sub = text(client.open().subscriptionId)
    client.call("ViewSetVisibleRequest", subscriptionId=sub, cards=[path("slow"), path("fast")])
    await_views(client)
    await_views(client)

    def receive(card, view, timeout=15):
        def matches(note):
            if note.valueType != NotificationPayload.DisplayUpdated:
                return False
            display = note.value.display
            return display.card.segments == [card.encode()] and text(display.view) == view
        return client.wait_notification(matches, timeout=timeout).value.display

    def release(display):
        client.call("ViewCreditRequest", subscriptionId=sub, card=display.card,
                    view=text(display.view), bytes=len(display.bytes), creditId=display.creditId)

    run_id = run(client, "start")
    first, second = receive("slow", "notes"), receive("slow", "notes")
    assert len(first.bytes) == len(second.bytes) == 1 << 24
    assert bytes(receive("slow", "plot").bytes) == b"\x08"
    assert bytes(receive("fast", "plot").bytes) == b"fast"
    with pytest.raises(TimeoutError):
        receive("slow", "plot", timeout=0.25)
    release(first)
    assert bytes(receive("slow", "plot").bytes) == b"\x09"
    release(second)
    for _ in range(7):
        release(receive("slow", "notes"))
    assert wait_run(client, run_id).state == RunState.succeeded


def test_full_burst_is_discarded_without_ui(project, tmp_path, core_factory):
    package = native_card(tmp_path / "burst", BURST)
    client = core_factory(mocked_builds=True).connect()
    Project(project).card("work", {"type": "code", "sandbox": UNISOLATED,
                                   "timeoutSeconds": 20}, outputs=OUTPUTS,
                          plan=mock_plan([package], views=VIEWS)).save(client)
    assert wait_run(client, run(client)).state == RunState.succeeded
    assert not any(note.valueType == NotificationPayload.DisplayUpdated
                   for note in client.notifications.copy())
