import json

import pytest

from Tsugumi.Trace.Client import Client
from tsugumi_client import RpcFailure, change, new_id, text


@pytest.mark.parametrize("transport", ["unix", "websocket"])
def test_hello_open_save(daemon, transport):
    """A client can open an empty project and save its first change."""
    client = daemon.connect(websocket=transport == "websocket")
    snap_open = client.open()
    assert snap_open.seq == 1
    rev = snap_open.snapshot.rev
    assert rev == 0
    doc = json.loads(text(snap_open.snapshot.projectJson))
    doc["name"] = "renamed"
    res = client.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="test",
                      changes=[change("SetProject", json=json.dumps(doc))])
    assert res.rev == 1 and res.delta.baseRev == 0


def test_one_ui_can_open_the_project(daemon):
    web = daemon.connect(websocket=True, client=Client.Web)
    mac = daemon.connect(client=Client.MacOS)
    mac.open()
    with pytest.raises(RpcFailure) as rejected:
        web.open()
    assert rejected.value.kind == "project_in_use"
    daemon.connect(client=Client.Host).open()
    daemon.connect().open()
    assert mac.open().seq == 1
    mac.close()
    # A new request on the same Web connection can acquire the released slot.
    # The server still needs to observe EOF on the Mac connection.
    import time
    deadline = time.monotonic() + 5
    while True:
        try:
            assert web.open().seq == 1
            break
        except RpcFailure as failure:
            if failure.kind != "project_in_use" or time.monotonic() >= deadline:
                raise
            time.sleep(0.01)
