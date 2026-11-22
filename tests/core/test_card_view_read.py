"""CardViewRead serves the bytes of a declared view file in pages, and nothing else."""

import pytest

from harness.scenario import Project
from tsugumi_client import RpcFailure, path, text
from Tsugumi.RPC.NotificationPayload import NotificationPayload

# NUL, non-UTF-8 and HTML bytes: the core relays them without looking.
CONTENT = b"<!doctype html>\x00\xff\xfe" + bytes(range(256)) * 9


def read_view(client, name, *, length):
    pieces, offset = [], 0
    while True:
        page = client.call("CardViewReadRequest", card=path("work"), name=name,
                           offset=offset, length=length)
        assert len(page.bytes) <= length
        pieces.append(bytes(page.bytes))
        offset += len(page.bytes)
        if offset >= page.total:
            return b"".join(pieces), page.total


def rejected(client, kind, **fields):
    with pytest.raises(RpcFailure) as failure:
        client.call("CardViewReadRequest", card=path("work"), **fields)
    assert failure.value.kind == kind


def test_files_that_were_not_declared_are_not_served(project, core_factory):
    """A Card's own files and absolute paths are not views unless declared."""
    client = core_factory().connect()
    Project(project).card("work", {"type": "code"},
                          sources={"page.html": "<p>not declared</p>"}).save(client)
    for name in ("page.html", "definitions/work/page.html",
                 str(project / "definitions" / "work" / "page.html")):
        rejected(client, "not_found", name=name)


@pytest.mark.xfail(strict=True, reason="needs the Nix lane's views list and the tsugumid "
                   "lane's CardViewsUpdated publishing (docs/dev/abi9-lanes.md)")
def test_declared_view_is_served_in_pages(project, tmp_path, core_factory):
    """The declared blob is read by name in pages; other names stay rejected."""
    blob = tmp_path / "store" / "page.html"
    blob.parent.mkdir()
    blob.write_bytes(CONTENT)
    other = tmp_path / "store" / "other.html"
    other.write_bytes(b"not declared")
    package = tmp_path / "package"
    package.mkdir()
    client = core_factory(mocked_builds=True).connect()
    Project(project).card("work", {"type": "code"}, plan={"product": {
        "runs": [str(package)],
        "views": [{"name": "page", "type": "html", "blob": str(blob)}],
    }}).save(client)
    opened = client.open()
    client.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId),
                cards=[path("work")])
    client.wait_notification(lambda n: n.valueType == NotificationPayload.CardViewsUpdated
                             and n.value.views, timeout=20)

    whole = client.call("CardViewReadRequest", card=path("work"), name="page")
    assert bytes(whole.bytes) == CONTENT and whole.total == len(CONTENT)
    assert read_view(client, "page", length=1000) == (CONTENT, len(CONTENT))
    tail = client.call("CardViewReadRequest", card=path("work"), name="page",
                       offset=len(CONTENT) - 3, length=1000)
    assert bytes(tail.bytes) == CONTENT[-3:] and tail.total == len(CONTENT)
    end = client.call("CardViewReadRequest", card=path("work"), name="page", offset=len(CONTENT))
    assert bytes(end.bytes) == b"" and end.total == len(CONTENT)

    rejected(client, "invalid_range", name="page", offset=len(CONTENT) + 1)
    rejected(client, "invalid_request", name="page", length=0)
    rejected(client, "invalid_request", name="page", length=(1 << 20) + 1)
    for name in ("other", "Page", str(blob), str(other)):
        rejected(client, "not_found", name=name)
