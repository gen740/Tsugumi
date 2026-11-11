import json

import pytest

from tsugumi_client import change, new_id, text


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
