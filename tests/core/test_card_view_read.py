"""The presenting subscription reads pages from the pinned view blob."""

import pytest

from harness.fixtures import mock_plan
from harness.native_card import native_card
from harness.scenario import Project, run
from harness.views import EMPTY_OUTPUTS, NATIVE_VIEW_SETTINGS, declaration, open_view
from projects import wait_run
from Tsugumi.Data.LogStream import LogStream
from Tsugumi.Data.RunState import RunState
from Tsugumi.Trace.Client import Client
from tsugumi_client import RpcFailure, new_id, path, text

CONTENT = b"<!doctype html>\x00\xff\xfe" + bytes(range(256)) * 9

OPEN = """
int tsugumi_main(const tsugumi_call *call, tsugumi_bytes *result) {
    tsugumi_route route;
    tsugumi_view_session session;
    if (call->effects->view_open(call->host, "page", TSUGUMI_VIEW_PANEL,
                                 0, 0, 0, &route, &session))
        return 1;
    return write_result(call, result, result_bytes, sizeof(result_bytes));
}
"""


def rejected(client, kind, session, **fields):
    with pytest.raises(RpcFailure) as failure:
        client.call("CardViewReadRequest", session=session, **fields)
    assert failure.value.kind == kind


def test_window_pins_the_declaration_and_blob_after_the_process_ends(project, tmp_path, core_factory):
    store = tmp_path / "store"
    store.mkdir()
    blob = store / "page.html"
    blob.write_bytes(CONTENT)
    package = native_card(tmp_path / "card", OPEN)
    daemon = core_factory(mocked_builds=True, env={"TSUGUMI_MOCK_STORE": str(store)})
    client = daemon.connect(client=Client.Web)
    graph = Project(project)
    graph.card("work", NATIVE_VIEW_SETTINGS,
               outputs=EMPTY_OUTPUTS, plan=mock_plan([package], views=[declaration(blob)])).save(client)
    client.open()
    run_id = run(client)
    session = text(open_view(client).session)
    assert wait_run(client, run_id).state == RunState.succeeded

    other = daemon.connect()
    other.open()
    rejected(other, "permission_denied", session)
    rejected(client, "not_found", new_id())
    rejected(client, "invalid_request", str(blob))
    rejected(client, "invalid_request", session, limitBytes=0)
    rejected(client, "invalid_request", session, limitBytes=(1 << 20) + 1)
    rejected(client, "invalid_range", session, offset=len(CONTENT) + 1)

    # Simulate a definition change and removal of the old store entry. The
    # session still serves its original bytes through the open descriptor.
    new_blob = store / "replacement.html"
    new_blob.write_bytes(b"replacement")
    graph.card("work", NATIVE_VIEW_SETTINGS, outputs=EMPTY_OUTPUTS,
               plan=mock_plan([package], views=[declaration(new_blob)])).save(client)
    blob.unlink()
    pieces = []
    offset = 0
    while True:
        page = client.call("CardViewReadRequest", session=session, offset=offset, limitBytes=1000)
        assert len(page.bytes) <= 1000 and page.offset == offset
        assert text(page.name) == "page" and text(page.hash) == str(blob)
        assert page.sizeBytes == len(CONTENT)
        pieces.append(bytes(page.bytes))
        if page.eof:
            break
        assert page.nextOffset == offset + len(page.bytes)
        offset = page.nextOffset
    assert b"".join(pieces) == CONTENT
    end = client.call("CardViewReadRequest", session=session, offset=len(CONTENT))
    assert bytes(end.bytes) == b"" and end.eof

    client.call("ViewCloseRequest", session=session)
    rejected(client, "not_found", session)


@pytest.mark.parametrize("invalid", ["outside", "symlink", "directory", "duplicate"])
def test_view_open_rejects_unserveable_declarations(project, tmp_path, core_factory, invalid):
    store = tmp_path / "store"
    store.mkdir()
    outside = tmp_path / "outside.html"
    outside.write_bytes(b"outside")
    escaped = store / "escaped.html"
    escaped.symlink_to(outside)
    inside = store / "page.html"
    inside.write_bytes(CONTENT)
    # Let the invocation succeed only when the requested view cannot open.
    package = native_card(tmp_path / "card", OPEN.replace("if (call->", "if (!call->"))
    daemon = core_factory(mocked_builds=True, env={"TSUGUMI_MOCK_STORE": str(store)})
    client = daemon.connect(client=Client.Web)
    blob = {"outside": outside, "symlink": escaped, "directory": store, "duplicate": inside}[invalid]
    views = [declaration(blob)] * (2 if invalid == "duplicate" else 1)
    Project(project).card("work", NATIVE_VIEW_SETTINGS,
                          outputs=EMPTY_OUTPUTS, plan=mock_plan([package], views=views)).save(client)
    client.open()
    run_id = run(client)
    status = wait_run(client, run_id)
    if status.state != RunState.succeeded:
        record = client.call("RunRecordRequest", runId=run_id, card=path("work")).record
        failure = record.summary.failure or status.failure
        stderr = client.call("LogRangeRequest", runId=run_id, card=path("work"),
                             invocation=1, stream=LogStream.output)
        pytest.fail(f"{failure and text(failure.diagnostic)}: {text(stderr.text)}")
