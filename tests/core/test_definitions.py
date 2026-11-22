"""Definition identity, nested projects and layout are observed through public RPC."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

from harness.values import recorded_input_json
from projects import MSG_FBS, SINK, builtin_project, card, conn, put, run_and_wait
from tsugumi_client import RpcFailure, change, new_id, path, text
from Tsugumi.Data.RunState import RunState
from harness.document import layout_bytes, write_layout


def project_json(c) -> tuple[int, dict]:
    snap = c.call("ProjectInspectRequest")
    return snap.rev, json.loads(text(snap.projectJson))


def diagnostics(c, card: list[str]) -> list[str]:
    snap = c.call("ProjectInspectRequest")
    for r in snap.resolution:
        if [text(s) for s in r.card.segments] == card:
            return [text(d.code) for d in r.diagnostics]
    raise KeyError(card)


def external(root: Path, name: str, text_value: str) -> None:
    """externals/<name>: an Input with its message as the boundary output."""
    ext = root / "externals" / name
    put(ext / "tsugumi.json", json.dumps({
        "name": name, "pkgsPath": "tsugumi:default",
        "canvas": {"cards": [{"id": "say"}], "connections": []},
        "boundary": {"inputs": [], "outputs": [{"name": "said", "card": "say", "port": "msg"}]}}))
    put(ext / "definitions" / "say" / "_card.json", json.dumps(
        {"type": "input", "params": [{"name": "text", "type": "string", "data": {"string": text_value}},
                                         {"name": "value", "type": "bool", "data": {"bool": True}}]}))
    put(ext / "definitions" / "say" / "_outputs.fbs", MSG_FBS)


def edit_description(c, card_id, description, prefix=""):
    """Edit the Card README without changing its identity."""
    snap = c.call("ProjectInspectRequest")
    readme = f"---\ndescription: {description}\n---\n"
    if not prefix:
        d = next(d for d in snap.definitions if text(d.id) == card_id)
        changes = [change("PutDefinition", id=card_id, cardJson=text(d.cardJson),
                          readme=readme, keepComments=True)]
    else:
        filename = prefix + "definitions/" + card_id + "/README.md"
        hashes = {text(s.path): text(s.hash) for s in snap.sources}
        changes = [change("PutSource", path=filename, bytes=readme.encode(),
                          baseHash=hashes.get(filename))]
    return c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="agent", changes=changes)


def recorded_invocations(client, run_id, *card_id):
    page = client.call("RunInvocationsRequest", runId=run_id, card=path(*card_id))
    return {record.invocation: record.state for record in page.records}


def test_description_edit_preserves_every_past_record_and_saved_input(tmp_path, core_factory):
    """A description edit preserves recorded invocations and saved inputs."""
    root = tmp_path / "p"
    builtin_project(root, text="hi")
    put(root / "definitions" / "out" / "_card.json", json.dumps(dict(SINK, recordIO=True)))
    put(root / "definitions" / "src" / "notes.txt", "kept")
    c = core_factory(project_root=root).connect()
    run_id, st = run_and_wait(c, "src")
    assert st.state == RunState.succeeded
    snap = c.call("ProjectInspectRequest")
    before = recorded_invocations(c, run_id, "src")
    assert before
    saved_input = recorded_input_json(c, tmp_path / "before", run_id, "out")
    result = edit_description(c, "src", "日本語 / source")
    assert result.executionRev == snap.executionRev
    assert recorded_invocations(c, run_id, "src") == before
    assert recorded_input_json(c, tmp_path / "after", run_id, "out") == saved_input
    assert json.loads(text(c.call("ProjectInspectRequest").projectJson)) == json.loads(text(snap.projectJson))
    assert bytes(c.call("ProjectFileReadRequest", path="definitions/src/notes.txt").bytes) == b"kept"
    assert c.call("RunRecordRequest", runId=run_id, card=path("src")).record
    assert c.call("RunInvocationsRequest", runId=run_id, card=path("src")).records
    run2, st2 = run_and_wait(c, "out", propagate=False)
    assert st2.state == RunState.succeeded
    assert recorded_input_json(c, tmp_path / "decode", run2, "out")["text"] == "hi"


def test_new_ids_are_unique_with_shared_descriptions(tmp_path, core_factory):
    """The core reserves short ids, including before any definitions are saved."""
    root = tmp_path / "p"
    builtin_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    ids = [text(c.call("CardIdRequest", stem="aggregate").id) for _ in range(80)]
    assert len(set(ids)) == len(ids)
    snap = c.call("ProjectInspectRequest")
    doc = json.loads(text(snap.projectJson))
    doc["canvas"]["cards"] += [{"id": x} for x in ids[:2]]
    c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="ui:test", changes=[
        change("SetProject", json=json.dumps(doc)), *[
            change("PutDefinition", id=x, cardJson='{"type":"draft"}', readme="---\ndescription: Aggregate\n---\n") for x in ids[:2]]])
    snap = c.call("ProjectInspectRequest")
    assert sum("description: Aggregate" in (text(x.readme) or "") for x in snap.definitions) == 2
    assert "src" in [text(x.id) for x in snap.definitions]  # existing ids are unchanged
    d.restart()
    c = d.connect()
    assert text(c.call("CardIdRequest", stem="aggregate").id) not in ids
    nested = text(c.call("CardIdRequest", stem="aggregate", projectPath="externals/new/").id)
    assert nested == "aggregate"
    for bad in ("../", "externals/../", "definitions/a/", "externals/new"):
        with pytest.raises(RpcFailure):
            c.call("CardIdRequest", stem="aggregate", projectPath=bad)


LAID_OUT = {
    "flow": "topToBottom",
    "cards": {"start": {"x": 0, "y": 0}, "src": {"x": 10.5, "y": 20, "collapsed": True}, "out": {"x": 30, "y": 40},
              "src/inner": {"x": 1, "y": 2}, "src2": {"x": 5, "y": 6}},
    "notes": [{"id": "n", "scope": "", "text": "メモ \"src\"\n改行", "x": 1, "y": 2}],
    "groups": [{"id": "g", "scope": "", "name": "src", "color": "blue", "cards": ["src", "out"],
                "outputs": [{"card": "src", "port": "msg"}, {"card": "out", "port": "x"}]}],
}


def laid_out_project(root: Path, layout: dict = LAID_OUT) -> None:
    builtin_project(root, extra_cards=[card("src2", "sink")])
    write_layout(root, layout)


def without_card(doc: dict, id: str) -> dict:
    doc["canvas"]["cards"] = [c for c in doc["canvas"]["cards"] if c["id"] != id]
    doc["canvas"]["connections"] = [e for e in doc["canvas"]["connections"] if id not in (e["from"]["card"], e["to"]["card"])]
    return doc


def test_an_agent_name_change_preserves_layout_verbatim(tmp_path, core_factory):
    """Names do not affect positions, groups or any formatting."""
    root = tmp_path / "p"
    laid_out_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    result = edit_description(c, "src", "source")
    assert bytes(c.call("ProjectInspectRequest").layout) == layout_bytes(LAID_OUT)
    assert result.delta.layoutChanged is False
    d.restart()
    assert bytes(d.connect().call("ProjectInspectRequest").layout) == layout_bytes(LAID_OUT)


def test_the_saving_client_supplies_deleted_card_layout_cleanup(tmp_path, core_factory):
    """The saving client supplies position and group cleanup atomically.

    A new card with the same name starts without that layout, while restoring
    the card and its layout together keeps the supplied layout.
    """
    root = tmp_path / "p"
    laid_out_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    rev, doc = project_json(c)
    kept = json.loads(json.dumps(LAID_OUT))
    kept["cards"] = {k: v for k, v in kept["cards"].items() if k not in ("src", "src/inner")}
    kept["groups"][0]["cards"] = ["out"]
    kept["groups"][0]["outputs"] = [{"card": "out", "port": "x"}]
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetProject", json=json.dumps(without_card(doc, "src"))), change("RemoveDefinition", id="src"), change("SetLayout", layout=layout_bytes(kept))])
    assert bytes(c.call("ProjectInspectRequest").layout) == layout_bytes(kept)

    # Off the canvas with its definition left behind is deleted too.
    rev, doc = project_json(c)
    del kept["cards"]["src2"]
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetProject", json=json.dumps(without_card(doc, "src2"))), change("SetLayout", layout=layout_bytes(kept))])
    assert bytes(c.call("ProjectInspectRequest").layout) == layout_bytes(kept)

    # A Card of the same name starts without the prior layout.
    rev, doc = project_json(c)
    doc["canvas"]["cards"].append({"id": "src"})
    res = c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("PutDefinition", id="src", cardJson=json.dumps(SINK)),
        change("SetProject", json=json.dumps(doc))])
    assert res.delta.layoutChanged is False
    assert bytes(c.call("ProjectInspectRequest").layout) == layout_bytes(kept)

    # Nothing of the Card in the layout: not rewritten, even in another form.
    rev, doc = project_json(c)
    compact = layout_bytes({"cards": {"out": {"x": 1, "y": 2}}})
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetLayout", layout=compact)])
    rev, doc = project_json(c)
    res = c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="agent", changes=[
        change("SetProject", json=json.dumps(without_card(doc, "src"))), change("RemoveDefinition", id="src")])
    assert res.delta.layoutChanged is False
    assert bytes(c.call("ProjectInspectRequest").layout) == compact

    # An undo: the Card comes back, with its layout, in one save.
    rev, doc = project_json(c)
    doc["canvas"]["cards"].append({"id": "src"})
    back = layout_bytes({"cards": {"src": {"x": 5, "y": 6}, "out": {"x": 1, "y": 2}}})
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="ui:x", changes=[
        change("PutDefinition", id="src", cardJson=json.dumps(SINK)),
        change("SetProject", json=json.dumps(doc)),
        change("SetLayout", layout=back)])
    assert bytes(c.call("ProjectInspectRequest").layout) == back


def test_every_card_needs_its_own_definition(tmp_path, core_factory):
    """A canvas cannot reference a missing or shared definition."""
    root = tmp_path / "p"
    builtin_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    rev, doc = project_json(c)
    doc["canvas"]["cards"].append({"id": "nobody"})
    with pytest.raises(RpcFailure) as err:
        c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t",
               changes=[change("SetProject", json=json.dumps(doc))])
    assert err.value.kind == "invalid_document"
    doc["canvas"]["cards"][-1] = {"id": "x", "definition": "src"}  # sharing is gone
    with pytest.raises(RpcFailure) as err:
        c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t",
               changes=[change("SetProject", json=json.dumps(doc))])
    assert err.value.kind == "invalid_document"


def test_external_documents_share_the_snapshot_and_delta_revision(tmp_path, core_factory):
    root = tmp_path / "p"
    builtin_project(root)
    prefix = "externals/outer/externals/inner/"
    project_path = prefix + "tsugumi.json"
    card_path = prefix + "definitions/definitions/_card.json"
    meta_path = prefix + "definitions/definitions/README.md"
    code_path = prefix + "definitions/definitions/src/run.py"
    project = '{"canvas":{"cards":[{"id":"definitions"}],"connections":[]}}'
    draft = '{"type":"draft"}'
    metadata = "---\ndescription: Nested Card\n---\n"
    root_doc = json.loads((root / "tsugumi.json").read_text())
    root_doc["canvas"]["cards"].append({"id": "outer"})
    put(root / "tsugumi.json", json.dumps(root_doc))
    put(root / "definitions/outer/_card.json", '{"type":"subgraph","external":"outer"}')
    outer = "externals/outer/"
    put(root / outer / "tsugumi.json",
        '{"canvas":{"cards":[{"id":"inner"}],"connections":[]}}')
    put(root / outer / "definitions/inner/_card.json", '{"type":"subgraph","external":"inner"}')
    for path, value in [(project_path, project), (card_path, draft),
                        (meta_path, metadata), (code_path, "# code")]:
        put(root / path, value)
    c = core_factory(project_root=root).connect()
    snap = c.call("ProjectInspectRequest")
    sources = {text(s.path): s for s in snap.sources}
    assert json.loads(bytes(sources[project_path].document)) == json.loads(project)
    assert json.loads(bytes(sources[card_path].document)) == json.loads(draft)
    assert bytes(sources[meta_path].document).decode() == metadata
    assert sources[code_path].document is None

    updated = '{"type":"input"}'
    saved = c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
                   changes=[
                       change("PutSource", path=card_path, bytes=updated.encode(),
                              baseHash=text(sources[card_path].hash)),
                       change("RemoveSource", path=meta_path, baseHash=text(sources[meta_path].hash)),
                   ])
    assert {text(s.path): json.loads(bytes(s.document)) for s in saved.delta.sourcesUpsert} == {card_path: json.loads(updated)}
    assert [text(p) for p in saved.delta.sourcesRemoved] == [meta_path]

    # Only a reload accepts an out-of-band change. A save that needs a new
    # graph resolution still sees the document that was accepted.
    nested_card = ["outer", "inner", "definitions"]
    accepted = diagnostics(c, nested_card)
    assert "invalid_document" not in accepted
    put(root / card_path, draft)
    layout = c.call("ProjectSaveRequest", baseRev=saved.rev, opId=new_id(), origin="t",
                    changes=[change("SetLayout", layout=layout_bytes({"flow": "topToBottom"}))])
    assert layout.executionRev == saved.executionRev
    assert diagnostics(c, nested_card) == accepted
    changed = c.call("ProjectSaveRequest", baseRev=layout.rev, opId=new_id(), origin="t",
                     changes=[change("PutDefinition", id="unrelated",
                                     cardJson='{"type":"draft"}')])
    assert diagnostics(c, nested_card) == accepted
    inspected = c.call("ProjectInspectRequest")
    assert inspected.rev == changed.rev
    accepted_source = next(s for s in inspected.sources if text(s.path) == card_path)
    assert json.loads(bytes(accepted_source.document)) == json.loads(updated)
    content = c.call("ProjectFileReadRequest", path=card_path, atRev=changed.rev)
    assert bytes(content.bytes) == updated.encode()
    with pytest.raises(RpcFailure) as error:
        c.call("ProjectSaveRequest", baseRev=changed.rev, opId=new_id(), origin="t",
               changes=[change("PutSource", path=card_path, bytes=b'{"type":"trigger"}',
                               baseHash=text(accepted_source.hash))])
    assert error.value.kind == "external_change"
    reloaded = c.call("ProjectReloadRequest", baseRev=changed.rev, opId=new_id(), origin="t")
    assert reloaded.executionRev > changed.executionRev
    assert {text(s.path): json.loads(bytes(s.document)) for s in reloaded.delta.sourcesUpsert} == {card_path: json.loads(draft)}


def test_card_in_card_is_an_external_project(tmp_path, core_factory):
    """A linked project can run as a nested card and as a standalone project."""
    root = tmp_path / "p"
    builtin_project(root)
    external(root, "greeting", "from the external")
    put(root / "externals/greeting/.git/config", "# excluded Git metadata")
    put(root / "definitions" / "greet" / "_card.json", json.dumps({"type": "subgraph", "external": "greeting"}))
    project = json.loads((root / "tsugumi.json").read_text())
    project["canvas"]["cards"].append({"id": "greet"})
    project["canvas"]["cards"].append({"id": "show"})
    put(root / "definitions" / "show" / "_card.json", json.dumps(dict(SINK, recordIO=True)))
    project["canvas"]["connections"].append(conn("greet", "said", "show", "in"))
    put(root / "tsugumi.json", json.dumps(project))
    d = core_factory(project_root=root)
    c = d.connect()
    snap = c.call("ProjectInspectRequest")
    cards = {"/".join(text(s) for s in r.card.segments) for r in snap.resolution}
    assert {"greet", "greet/say"} <= cards
    assert any(text(x.id) == "greet" and json.loads(text(x.cardJson)).get("external") == "greeting" for x in snap.definitions)
    first = c.call("VersionInitRequest", opId=new_id(), origin="test")
    source = "externals/greeting/definitions/say/_card.json"
    body = json.loads((root / source).read_text())
    body["params"][0]["data"] = {"string": "changed on disk"}
    put(root / source, json.dumps(body))
    with pytest.raises(RpcFailure) as error:
        c.call("RunRequest", baseRev=snap.rev, opId=new_id(), origin="test", card=path("greet"))
    assert error.value.kind == "external_change"
    assert c.call("RunStatusRequest").runId is None
    assert subprocess.check_output(["git", "-C", root, "rev-parse", "HEAD"], text=True).strip() == text(first.commit)
    reloaded = c.call("ProjectReloadRequest", baseRev=snap.rev,
                      opId=new_id(), origin="test")
    run_id, st = run_and_wait(c, "greet")
    assert st.state == RunState.succeeded, st.failure and text(st.failure.diagnostic)
    committed = subprocess.check_output(
        ["git", "-C", root, "show", f"{text(st.commit)}:{source}"], text=True)
    result = recorded_input_json(c, tmp_path / "decode", run_id, "show")
    assert result["text"] == json.loads(committed)["params"][0]["data"]["string"] == "changed on disk"
    # The external is this project's copy: its files change here like any other.
    rev = reloaded.rev
    c.call("ProjectSaveRequest", baseRev=rev, opId=new_id(), origin="t",
           changes=[change("PutSource", path="externals/greeting/notes.txt", bytes=list(b"x"))])
    assert bytes(c.call("ProjectFileReadRequest", path="externals/greeting/notes.txt").bytes) == b"x"
    snap = c.call("ProjectInspectRequest")
    c.call("ProjectSaveRequest", baseRev=snap.rev, opId=new_id(), origin="t",
           changes=[change("PutSource", path="externals/fresh/tsugumi.json", bytes=list(b"{}"))])
    # The external opens on its own, with the same result.
    d.stop()
    alone = root / "externals" / "greeting"
    (alone / ".git/config").unlink()
    (alone / ".git").rmdir()
    d = core_factory(project_root=alone)
    c = d.connect()
    run_id, st = run_and_wait(c, "say")
    assert st.state == RunState.succeeded


def test_link_diagnostics(tmp_path, core_factory):
    """Missing external cards are diagnosed and links outside the project are rejected."""
    root = tmp_path / "p"
    builtin_project(root)
    put(root / "definitions" / "missing" / "_card.json", json.dumps({"type": "subgraph", "external": "nowhere"}))
    project = json.loads((root / "tsugumi.json").read_text())
    project["canvas"]["cards"].append({"id": "missing"})
    put(root / "tsugumi.json", json.dumps(project))
    c = core_factory(project_root=root).connect()
    assert "unknown_external" in diagnostics(c, ["missing"])
    before = c.call("ProjectInspectRequest")
    with pytest.raises(RpcFailure) as failure:
        c.call("ProjectSaveRequest", baseRev=before.rev, opId=new_id(), origin="ui:test", changes=[
            change("PutDefinition", id="bad", cardJson=json.dumps({"type": "subgraph", "external": "../up"}))])
    assert failure.value.kind == "invalid_document"
    after = c.call("ProjectInspectRequest")
    assert after.rev == before.rev
    assert {text(d.id) for d in after.definitions} == {text(d.id) for d in before.definitions}


@pytest.mark.parametrize("settings", [{"maxRuns": 2}, {"grants": {"network": False}}])
def test_card_in_card_definition_rejects_execution_settings(tmp_path, core_factory, settings):
    root = tmp_path / "p"
    builtin_project(root)
    c = core_factory(project_root=root).connect()
    before = c.call("ProjectInspectRequest")
    invalid = json.dumps({"type": "subgraph", "external": "inner", **settings})
    for write in (change("PutDefinition", id="nested", cardJson=invalid),
                  change("PutSource", path="externals/inner/definitions/nested/_card.json",
                         bytes=list(invalid.encode()))):
        with pytest.raises(RpcFailure) as error:
            c.call("ProjectSaveRequest", baseRev=before.rev, origin="test",
                   opId=new_id(), changes=[write])
        assert error.value.kind == "invalid_document"
        assert c.call("ProjectInspectRequest").rev == before.rev


def test_display_mode_setting(tmp_path, core_factory):
    """Presentation modes do not participate in core validation."""
    for value, reuse_worker, valid in (("update", False, True), ("live", True, True), ("smooth", True, True)):
        root = tmp_path / value
        builtin_project(root)
        card = json.loads((root / "definitions" / "out" / "_card.json").read_text())
        card["displayMode"] = value
        card["reuseWorker"] = reuse_worker
        put(root / "definitions" / "out" / "_card.json", json.dumps(card))
        c = core_factory(project_root=root).connect()
        assert ("invalid_setting" not in diagnostics(c, ["out"])) == valid, value


def test_output_types_lists_and_rewrites_a_port(tmp_path, core_factory):
    """Output type edits preserve unrelated schema text and reject incompatible types."""
    root = tmp_path / "p"
    builtin_project(root)
    d = core_factory(project_root=root)
    c = d.connect()
    fbs = ("namespace demo;\n// kept\ntable Msg { text:string; }\ntable Path { value:string; }\n"
           "table Outputs { msg:Msg (required); /* note */ other:Msg; }\nroot_type Outputs;\n")
    res = c.call("OutputTypesRequest", text=fbs)
    assert text(res.rootType) == "demo.Outputs" and len(bytes(res.bfbs)) > 0 and res.text is None
    changed = c.call("OutputTypesRequest", text=fbs, port="msg", typeName="demo.Path", kind=1)  # Path
    out = text(changed.text)
    assert 'msg:string (required, tsugumi: "path");' in out and "other:Msg;" in out and "// kept" in out
    assert 'attribute "tsugumi";' in out
    bad = c.call("OutputTypesRequest", text=fbs, port="msg", typeName="demo.Missing")
    assert bad.text is None and bad.error
    restored = c.call("OutputTypesRequest", text=out, port="msg", typeName="demo.Msg", kind=2)
    assert 'msg:Msg (required);' in text(restored.text)



def test_name_changes_inside_a_card_in_card_preserve_history(tmp_path, core_factory):
    """Both container and inner names change without touching a past record."""
    root = tmp_path / "p"
    builtin_project(root)
    external(root, "greeting", "hello")
    put(root / "definitions/greet/_card.json", '{"type":"subgraph","external":"greeting"}')
    doc = json.loads((root / "tsugumi.json").read_text())
    doc["canvas"]["cards"].append({"id": "greet"})
    put(root / "tsugumi.json", json.dumps(doc))
    c = core_factory(project_root=root).connect()
    run_id, st = run_and_wait(c, "greet")
    assert st.state == RunState.succeeded
    snap = c.call("ProjectInspectRequest")
    before = recorded_invocations(c, run_id, "greet", "say")
    assert before
    edit_description(c, "say", "Greeting", "externals/greeting/")
    edit_description(c, "greet", "Greeting")
    assert recorded_invocations(c, run_id, "greet", "say") == before
    assert c.call("RunInvocationsRequest", runId=run_id, card=path("greet", "say")).records
    after = c.call("ProjectInspectRequest")
    assert after.executionRev == snap.executionRev
    assert ("greet", "say") in {tuple(text(s) for s in r.card.segments) for r in after.resolution}
    assert b"description: Greeting" in bytes(c.call("ProjectFileReadRequest", path="externals/greeting/definitions/say/README.md").bytes)
