"""CardViewRead serves the bytes of a declared view file in pages, and nothing else."""

import pytest

from harness.fixtures import mock_plan
from harness.scenario import Project
from tsugumi_client import RpcFailure, path, text
from Tsugumi.RPC.NotificationPayload import NotificationPayload

# NUL, non-UTF-8 and HTML bytes: the core relays them without looking.
CONTENT = b"<!doctype html>\x00\xff\xfe" + bytes(range(256)) * 9


def read_view(client, name, *, limit, index=0):
    pieces, offset = [], 0
    while True:
        page = client.call("CardViewReadRequest", card=path("work"), name=name, index=index,
                           offset=offset, limitBytes=limit)
        assert len(page.bytes) <= limit and page.offset == offset
        pieces.append(bytes(page.bytes))
        if page.eof:
            return b"".join(pieces), page.sizeBytes
        assert page.nextOffset == offset + len(page.bytes)
        offset = page.nextOffset


def store_env(tmp_path):
    """The mocked builds treat this folder as the Nix store."""
    return {"TSUGUMI_MOCK_STORE": str(tmp_path / "store")}


def rejected(client, kind, **fields):
    with pytest.raises(RpcFailure) as failure:
        client.call("CardViewReadRequest", card=path("work"), **{"index": 0, **fields})
    assert failure.value.kind == kind


def test_files_that_were_not_declared_are_not_served(project, core_factory):
    """A Card's own files and absolute paths are not views unless declared."""
    client = core_factory().connect()
    Project(project).card("work", {"type": "code"},
                          sources={"page.html": "<p>not declared</p>"}).save(client)
    for name in ("page.html", "definitions/work/page.html",
                 str(project / "definitions" / "work" / "page.html")):
        rejected(client, "not_found", name=name)


def test_declared_view_is_served_in_pages(project, tmp_path, core_factory):
    """The declared blob is read by name in pages; other names stay rejected."""
    blob = tmp_path / "store" / "page.html"
    blob.parent.mkdir()
    blob.write_bytes(CONTENT)
    other = tmp_path / "store" / "other.html"
    other.write_bytes(b"not declared")
    package = tmp_path / "package"
    package.mkdir()
    client = core_factory(mocked_builds=True, env=store_env(tmp_path)).connect()
    Project(project).card("work", {"type": "code"}, outputs="table Outputs {} root_type Outputs;",
                          plan=mock_plan([str(package)], views=[
                              {"blob": str(blob), "name": "page", "type": "html"}])).save(client)
    opened = client.open()
    client.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId),
                cards=[path("work")])
    client.wait_notification(lambda n: n.valueType == NotificationPayload.CardViewsUpdated
                             and n.value.views, timeout=20)

    whole = client.call("CardViewReadRequest", card=path("work"), name="page", index=0)
    assert bytes(whole.bytes) == CONTENT and whole.sizeBytes == len(CONTENT) and whole.eof
    assert text(whole.name) == "page" and text(whole.hash) == str(blob)
    assert read_view(client, "page", limit=1000) == (CONTENT, len(CONTENT))
    tail = client.call("CardViewReadRequest", card=path("work"), name="page", index=0,
                       offset=len(CONTENT) - 3, limitBytes=1000)
    assert bytes(tail.bytes) == CONTENT[-3:] and tail.eof
    end = client.call("CardViewReadRequest", card=path("work"), name="page", index=0,
                      offset=len(CONTENT))
    assert bytes(end.bytes) == b"" and end.eof and end.sizeBytes == len(CONTENT)

    rejected(client, "invalid_range", name="page", offset=len(CONTENT) + 1)
    rejected(client, "invalid_request", name="page", limitBytes=0)
    rejected(client, "invalid_request", name="page", limitBytes=(1 << 20) + 1)
    for name in ("other", "Page", str(blob), str(other)):
        rejected(client, "not_found", name=name)
    rejected(client, "not_found", name="page", index=1)
    with pytest.raises(RpcFailure) as failure:
        client.call("CardViewReadRequest", card=path("work"), name="page")
    assert failure.value.kind == "invalid_request"


def test_repeated_names_are_served_by_the_selected_declaration(project, tmp_path, core_factory):
    """Two declarations with one name keep their own blobs; the index selects one."""
    store = tmp_path / "store"
    store.mkdir()
    blobs = [store / "first.html", store / "second.html"]
    blobs[0].write_bytes(b"first declaration")
    blobs[1].write_bytes(b"second declaration")
    package = tmp_path / "package"
    package.mkdir()
    client = core_factory(mocked_builds=True, env=store_env(tmp_path)).connect()
    Project(project).card("work", {"type": "code"}, outputs="table Outputs {} root_type Outputs;",
                          plan=mock_plan([str(package)], views=[
                              {"blob": str(blob), "name": "page", "type": "html"} for blob in blobs])).save(client)
    opened = client.open()
    client.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId),
                cards=[path("work")])
    update = client.wait_notification(lambda n: n.valueType == NotificationPayload.CardViewsUpdated
                                      and n.value.views, timeout=20)
    assert [text(v.name) for v in update.value.views] == ["page", "page"]
    for index, blob in enumerate(blobs):
        assert read_view(client, "page", limit=1000, index=index) == (blob.read_bytes(), blob.stat().st_size)
    rejected(client, "not_found", name="page", index=2)


def test_a_view_file_is_served_only_from_inside_the_store(project, tmp_path, core_factory):
    """Links inside the store are followed; a link leading out of it, a folder
    and a file outside the store are not served."""
    store = tmp_path / "store"
    (store / "object").mkdir(parents=True)
    (store / "object" / "page.html").write_bytes(b"inside")
    (store / "joined.html").symlink_to(store / "object" / "page.html")
    outside = tmp_path / "outside.html"
    outside.write_bytes(b"outside the store")
    (store / "escape.html").symlink_to(outside)
    (store / "escape-dir").symlink_to(tmp_path)
    package = tmp_path / "package"
    package.mkdir()
    blobs = [store / "joined.html", store / "escape.html", store / "escape-dir" / "outside.html",
             store / "object", outside]
    client = core_factory(mocked_builds=True, env=store_env(tmp_path)).connect()
    Project(project).card("work", {"type": "code"}, outputs="table Outputs {} root_type Outputs;",
                          plan=mock_plan([str(package)], views=[
                              {"blob": str(blob), "name": f"v{index}", "type": "html"}
                              for index, blob in enumerate(blobs)])).save(client)
    opened = client.open()
    client.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId),
                cards=[path("work")])
    client.wait_notification(lambda n: n.valueType == NotificationPayload.CardViewsUpdated
                             and n.value.views, timeout=20)
    assert read_view(client, "v0", limit=1000, index=0) == (b"inside", 6)
    for index in range(1, len(blobs)):
        rejected(client, "not_found", name=f"v{index}", index=index)
