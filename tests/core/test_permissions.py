"""Permission comparisons and explicit approvals are independent of origin."""

from __future__ import annotations

import json

import pytest
from projects import builtin_project, card, card_in_card, write
from harness.permissions import permissions, save
from tsugumi_client import RpcFailure, change, new_id, text


def card_json(c, cid: str) -> dict:
    snap = c.call("ProjectInspectRequest")
    for d in snap.definitions:
        if text(d.id) == cid:
            return json.loads(text(d.cardJson))
    raise KeyError(cid)


def put(cid: str, body: dict):
    return change("PutDefinition", id=cid, cardJson=json.dumps(body), keepMeta=True)


def test_checked_save_widening_permissions_needs_approval(tmp_path, core_factory):
    """The same checked save requires every widening for all origin labels."""
    root = tmp_path / "p"
    builtin_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    body = card_json(c, "src")
    wide = dict(body, sandbox={"filesystem": False},
                grants={"network": True, "hostPaths": [{"path": "/data", "name": "data"}]})
    for origin in ("agent", "ui", "external"):
        with pytest.raises(RpcFailure) as rejected:
            save(c, [put("src", wide)], origin=origin, check=True)
        assert rejected.value.kind == "approval_required"
        asked = permissions(rejected.value)
        assert sorted(asked) == ["host_folder_read src /data", "network src", "sandbox_off src"]
    # Approving only part of it is not enough.
    try:
        save(c, [put("src", wide)], approved=asked[:1], origin="external", check=True)
        raise AssertionError("a partly approved widening went through")
    except RpcFailure as e:
        assert e.kind == "approval_required"
    save(c, [put("src", wide)], approved=asked, origin="external", check=True)
    assert card_json(c, "src")["grants"]["network"] is True
    # Making the folder writable is another widening; narrowing is not.
    writable = dict(wide, grants={**wide["grants"], "hostPaths": [{"path": "/data", "name": "data", "write": True}]})
    try:
        save(c, [put("src", writable)])
        raise AssertionError("write access without approval")
    except RpcFailure as e:
        assert permissions(e) == ["host_folder_write src /data"]
    save(c, [put("src", body)])
    assert "grants" not in card_json(c, "src")
    # With the same token and origin, explicit comparison off permits widening.
    save(c, [put("src", wide)], origin="external", check=False)


def test_reload_checks_every_permission_widening(tmp_path, core_factory):
    """External edits cannot bypass approval, including nested Cards and UI reloads."""
    root = tmp_path / "project"
    definition = {"type": "code", "language": "bash", "executionService": "local", "_outputs": "table Outputs {} root_type Outputs;",
                  "_nix": '{ pkgs, inputs, params }: { run = "true"; }'}
    work = card("work", "code")
    cards = [card_in_card("inner", [work], [], {"inputs": {}, "outputs": {}})]
    write(root, {"code": definition}, cards, [])
    rel = "externals/inner/definitions/work/_card.json"
    cid = "inner/work"
    client = core_factory(project_root=root).connect()
    before = client.call("ProjectInspectRequest")
    path = root / rel
    body = json.loads(path.read_text())
    wide = dict(body, executionService="ssh-test",
                sandbox={"filesystem": False, "pid": False, "limits": {"cpu": 1, "memoryMiB": 1024}},
                grants={"network": True, "gui": True,
                        "hostPaths": [{"path": "/data", "name": "data", "write": True}]})
    expected = {f"execution_service {cid} ssh-test", f"network {cid}",
                f"sandbox_off {cid}", f"sandbox_options {cid}", f"gui {cid}", f"host_folder_write {cid} /data"}
    path.write_text(json.dumps(wide))

    def reload(approved=(), *, check=True, origin="external"):
        return client.call("ProjectReloadRequest", baseRev=before.rev, opId=new_id(),
                           origin=origin, checkPermissions=check, approvedPermissions=list(approved))

    for origin in ("agent", "ui", "external"):
        with pytest.raises(RpcFailure) as rejected:
            reload(origin=origin)
        assert rejected.value.kind == "approval_required"
        assert set(permissions(rejected.value)) == expected
    assert client.call("ProjectInspectRequest").rev == before.rev
    # Neither disk nor the accepted document is replaced on rejection.
    assert json.loads(path.read_text()) == wide
    with pytest.raises(RpcFailure) as partial:
        reload(expected - {f"network {cid}"})
    assert partial.value.kind == "approval_required"
    reload(expected)
    assert client.call("ProjectInspectRequest").rev == before.rev + 1
    # Returning to local narrows the target; auto would allow other workers too.
    path.write_text(json.dumps(body))
    before = client.call("ProjectInspectRequest")
    with pytest.raises(RpcFailure) as unbounded:
        reload()
    assert permissions(unbounded.value) == [f"sandbox_options {cid}"]
    reload([f"sandbox_options {cid}"])

    before = client.call("ProjectInspectRequest")
    base_hash = next(text(source.hash) for source in before.sources if text(source.path) == rel)
    edit = change("PutSource", path=rel, bytes=json.dumps(wide).encode(), baseHash=base_hash)
    with pytest.raises(RpcFailure) as rejected:
        save(client, [edit])
    assert rejected.value.kind == "approval_required"
    assert set(permissions(rejected.value)) == expected

    path.write_text(json.dumps(wide))
    assert reload(check=False).rev == before.rev + 1
