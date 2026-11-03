"""A save from a model (origin "agent") that widens what a Card may do --
network, sandbox off, host folders, also inside a Card in Card -- is rejected
with approval_required naming each widening, and goes through once the client
lists exactly those as approved. The user's own saves need no approval."""

from __future__ import annotations

import json

from conftest import Daemon
from projects import builtin_project, card, card_in_card, conn, write
from tsugumi_client import RpcFailure, change, new_id, text


def permissions(e: RpcFailure) -> list[str]:
    return [text(kv.value) for kv in e.failure.args if text(kv.key) == "permission"]


def save(c, changes, origin="agent", approved=None):
    rev = c.call("ProjectInspectRequest").rev
    extra = {"approvedPermissions": approved} if approved is not None else {}
    return c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin=origin, changes=changes, **extra)


def card_json(c, cid: str) -> dict:
    snap = c.call("ProjectInspectRequest")
    for d in snap.definitions:
        if text(d.id) == cid:
            return json.loads(text(d.cardJson))
    raise KeyError(cid)


def put(cid: str, body: dict):
    return change("PutDefinition", id=cid, cardJson=json.dumps(body), keepMeta=True)


def test_an_agent_save_widening_permissions_needs_the_users_approval(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        body = card_json(c, "src")
        wide = dict(body, network=True, sandbox=False, hostFolders=[{"path": "/data", "name": "data"}])
        try:
            save(c, [put("src", wide)])
            raise AssertionError("an agent widened permissions without approval")
        except RpcFailure as e:
            assert e.kind == "approval_required", e
            asked = permissions(e)
        assert sorted(asked) == ["host_folder_read src /data", "network src", "sandbox_off src"]
        # Approving only part of it is not enough.
        try:
            save(c, [put("src", wide)], approved=asked[:1])
            raise AssertionError("a partly approved widening went through")
        except RpcFailure as e:
            assert e.kind == "approval_required"
        save(c, [put("src", wide)], approved=asked)
        assert card_json(c, "src")["network"] is True
        # Making the folder writable is another widening; narrowing is not.
        writable = dict(wide, hostFolders=[{"path": "/data", "name": "data", "write": True}])
        try:
            save(c, [put("src", writable)])
            raise AssertionError("write access without approval")
        except RpcFailure as e:
            assert permissions(e) == ["host_folder_write src /data"]
        save(c, [put("src", body)])
        assert "network" not in card_json(c, "src")
        # The user's own saves are theirs.
        save(c, [put("src", wide)], origin="ui")
    finally:
        d.cleanup()


def test_a_card_in_card_is_checked_too(tmp_path):
    root = tmp_path / "p"
    defs = {"src": {"template": "input", "params": [{"name": "text", "type": "string", "data": "x"}],
                    "_outputs": "namespace m; table Msg { text:string; } table Outputs { msg:Msg (required); } root_type Outputs;"},
            "log": {"template": "logger"}}
    inner = [card("src", "src"), card("out", "log")]
    write(root, defs, [card_in_card("inner", inner, [conn("src", "msg", "out", "in")],
                                     {"inputs": {}, "outputs": {"msg": {"card": "src", "port": "msg"}}})], [])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        rel = "externals/inner/definitions/src/_card.json"
        snap = c.call("ProjectInspectRequest")
        hashes = {text(s.path): text(s.hash) for s in snap.sources}
        body = json.loads((root / rel).read_text())
        body["network"] = True
        try:
            save(c, [change("PutSource", path=rel, bytes=list(json.dumps(body).encode()), baseHash=hashes[rel])])
            raise AssertionError("a Card in Card was widened without approval")
        except RpcFailure as e:
            assert permissions(e) == ["network inner/src"]
    finally:
        d.cleanup()
