"""Project reads and proposed changes through the Agent-facing RPCs."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from projects import SINK, put
from tsugumi_client import RpcFailure, change, new_id, path, text

EVENTS_FBS = "table Events { value:string; }\ntable Outputs { events:Events (required); }\nroot_type Outputs;\n"


def make(root: Path) -> None:
    put(root / "tsugumi.json", json.dumps({
        "name": "files", "nixPath": [],
        "canvas": {"cards": [{"id": "start"}, {"id": "load"}, {"id": "show"}], "connections": [
            {"from": {"card": "load", "port": "events"}, "to": {"card": "show", "port": "in"}}]}}))
    put(root / "definitions" / "start" / "_card.json", json.dumps({"type": "trigger"}))
    put(root / "definitions" / "load" / "_card.json", json.dumps({
        "type": "input"}))
    put(root / "definitions" / "load" / "params.nix", '{ value = "data/events.txt"; }')
    put(root / "definitions" / "load" / "default.nix", '{ params, inputs }: { }')
    put(root / "definitions" / "load" / "_outputs.fbs", EVENTS_FBS)
    put(root / "definitions" / "load" / "data" / "events.txt", "photon 1\nphoton 2\n")
    put(root / "definitions" / "show" / "_card.json", json.dumps(SINK))


def test_reads_without_at_rev_and_the_root_as_empty_path(tmp_path, core_factory):
    root = tmp_path / "p"
    make(root)
    c = core_factory(project_root=root, mocked_builds=True).connect()
    listed = c.call("ProjectFileListRequest", path="", depth=1, includeDirectories=True)
    names = {text(e.path) for e in listed.entries}
    assert "tsugumi.json" in names and "definitions" in names, names
    got = c.call("ProjectFileReadRequest", path="definitions/load/data/events.txt")
    assert bytes(got.bytes).decode().startswith("photon 1")


def test_validate_checks_proposed_changes_without_saving(tmp_path, core_factory):
    root = tmp_path / "p"
    make(root)
    c = core_factory(project_root=root, mocked_builds=True).connect()
    before = c.call("ProjectInspectRequest")
    bad = change("PutSource", path="definitions/load/_outputs.fbs", bytes=list(b"table Broken {"),
                 baseHash=next(text(s.hash) for s in before.sources if text(s.path) == "definitions/load/_outputs.fbs"))
    v = c.call("ProjectValidateRequest", targets=[path("load")], changes=[bad])
    assert not v.valid and v.diagnostics, "a broken _outputs.fbs is reported"
    after = c.call("ProjectInspectRequest")
    assert after.rev == before.rev, "nothing was saved"
    assert bytes(c.call("ProjectFileReadRequest", path="definitions/load/_outputs.fbs").bytes).decode() == EVENTS_FBS
    # The current document itself is valid.
    assert c.call("ProjectValidateRequest").valid


def test_put_definition_can_keep_the_readme(tmp_path, core_factory):
    """Settings change without writing the intent (README.md) again."""
    root = tmp_path / "p"
    make(root)
    readme = "---\ndescription: 観測データを読む\n---\n\n露光補正はしない"
    put(root / "definitions" / "load" / "README.md", readme)
    c = core_factory(project_root=root, mocked_builds=True).connect()
    snap = c.call("ProjectInspectRequest")
    card = {"type": "input", "maxRuns": 2}
    c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
           changes=[change("PutDefinition", id="load", cardJson=json.dumps(card), keepReadme=True, keepComments=True)])
    assert bytes(c.call("ProjectFileReadRequest", path="definitions/load/README.md").bytes).decode() == readme
    assert json.loads(bytes(c.call("ProjectFileReadRequest", path="definitions/load/_card.json").bytes))["maxRuns"] == 2


def test_param_literal_rewrite_preserves_bytes_and_checks_conflicts(tmp_path, core_factory):
    root = tmp_path / "p"
    make(root)
    source = root / "definitions/load/params.nix"
    original = b'{\r\n  # keep the file name\r\n  "value" /* key */ =  "a;#b"  ; # suffix\r\n  flag = true;\r\n}\r\n'
    source.write_bytes(original)
    c = core_factory(project_root=root, mocked_builds=True).connect()
    snap = c.call("ProjectInspectRequest")
    fields = dict(card=path("load"), name="value", atRev=snap.rev)
    for literal in ('22', '"other"; injection = "no"'):
        with pytest.raises(RpcFailure) as mismatch:
            c.call("RewriteParamLiteralRequest", literal=literal, **fields)
        assert mismatch.value.kind == "validation_failed"
    assert source.read_bytes() == original

    rewrite = c.call("RewriteParamLiteralRequest", literal='"after"', **fields)
    expected = original.replace(b'"a;#b"', b'"after"')
    assert rewrite.text == expected
    edit = change("PutSource", path=text(rewrite.path), baseHash=text(rewrite.baseHash), bytes=rewrite.text)
    source.write_bytes(original + b"# concurrent edit\n")
    with pytest.raises(RpcFailure) as conflict:
        c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="ui:test", changes=[edit])
    assert conflict.value.kind == "external_change"
    source.write_bytes(original)
    saved = c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="ui:test", changes=[edit])
    assert source.read_bytes() == expected
    node = next(n for n in saved.delta.resolution if n.card.segments == [b"load"])
    assert [(p.name, p.type, p.valueJson) for p in node.parameters] == [
        (b"flag", b"bool", b"true"), (b"value", b"string", b'"after"'),
    ]
    with pytest.raises(RpcFailure) as stale:
        c.call("RewriteParamLiteralRequest", literal='"stale"', **fields)
    assert stale.value.kind == "revision_conflict"
