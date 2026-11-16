"""Display derivation never writes; placement shares the adding save's revision."""
import json
import uuid

from harness.host import Host
from tsugumi_client import change as doc_change, text
from Tsugumi.RPC.ProjectSaveRequest import ProjectSaveRequestT


def register(host, daemon):
    host.call("RegisterProjectRequest", projectId=daemon.ready["project_id"],
              endpoint="unix:" + daemon.unix, token=daemon.ready["token"],
              path=str(daemon.project))


def display(host, project_json, layout="{}", definitions=None):
    return json.loads(text(host.call("LayoutCompleteRequest", projectJson=project_json,
        layoutJson=layout, definitionsJson=json.dumps(definitions or {})).json))


def save(host, daemon, client, snapshot, changes):
    request = ProjectSaveRequestT()
    request.baseRev = snapshot.rev
    request.opId = str(uuid.uuid4())
    request.origin = "user"
    request.changes = changes
    prepared = host.call("LayoutPrepareSaveRequest", project=daemon.ready["project_id"], save=request).save
    # Preparation itself is read-only, even when called repeatedly.
    assert client.call("ProjectInspectRequest").rev == snapshot.rev
    return client.call("ProjectSaveRequest", baseRev=prepared.baseRev, opId=prepared.opId,
                       origin=prepared.origin, changes=prepared.changes)


def documents(project):
    return {str(p.relative_to(project)): p.read_bytes() for p in project.rglob("*")
            if p.is_file() and ".tsugumi" not in p.relative_to(project).parts}


def test_open_notice_and_reload_derive_without_writing(core_factory, project, tmp_path):
    graph = json.loads((project / "tsugumi.json").read_text())
    graph["canvas"]["cards"] = [{"id": "opened"}]
    (project / "tsugumi.json").write_text(json.dumps(graph))
    definition = project / "definitions/opened"
    definition.mkdir(parents=True)
    (definition / "_card.json").write_text('{"template":"draft"}')
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    host = Host(tmp_path / "host")
    try:
        before = client.call("ProjectInspectRequest")
        files = documents(project)
        register(host, daemon)
        first = display(host, text(before.projectJson), text(before.layoutJson))
        # Two clients derive exactly the same display from the same document.
        assert display(host, text(before.projectJson), text(before.layoutJson)) == first
        assert "opened" in first["cards"]
        assert client.call("ProjectInspectRequest").rev == before.rev
        assert documents(project) == files
        # The very next person's edit uses the revision read before opening.
        result = save(host, daemon, client, before, [doc_change("PutSource",
            path="definitions/opened/note.txt", bytes=b"person's edit")])
        assert result.rev == before.rev + 1
        assert json.loads(text(result.delta.layoutJson))["cards"] == first["cards"]
        assert client.call("ProjectInspectRequest").rev == result.rev

        # A direct writer and a reload can introduce another absent place.
        graph["canvas"]["cards"].append({"id": "from-git"})
        (project / "tsugumi.json").write_text(json.dumps(graph))
        new = project / "definitions/from-git"
        new.mkdir(parents=True)
        (new / "_card.json").write_text('{"template":"draft"}')
        client.call("ProjectReloadRequest", baseRev=result.rev, opId=str(uuid.uuid4()), origin="user")
        reloaded = client.call("ProjectInspectRequest")
        files = documents(project)
        layout = display(host, text(reloaded.projectJson), text(reloaded.layoutJson))
        assert "from-git" in layout["cards"]
        assert layout["cards"]["opened"] == first["cards"]["opened"]
        assert client.call("ProjectInspectRequest").rev == reloaded.rev
        assert documents(project) == files
    finally:
        host.close()


def test_adding_cards_and_nested_documents_places_in_one_revision(core_factory, project, tmp_path):
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    host = Host(tmp_path / "host")
    try:
        register(host, daemon)
        snapshot = client.call("ProjectInspectRequest")
        graph = json.loads(text(snapshot.projectJson))
        graph["canvas"]["cards"] = [{"id": "dropped"}, {"id": "automatic"}, {"id": "outer"}]
        nested = json.dumps({"name": "Inner", "pkgsPath": "tsugumi:default", "boundary": {"inputs": {}, "outputs": {}},
            "canvas": {"cards": [{"id": "inside"}], "connections": []}})
        result = save(host, daemon, client, snapshot, [
            doc_change("SetProject", json=json.dumps(graph)),
            doc_change("PutDefinition", id="dropped", cardJson='{"template":"draft"}'),
            doc_change("PutDefinition", id="automatic", cardJson='{"template":"draft"}'),
            doc_change("PutDefinition", id="outer", cardJson="", linkJson='{"external":"inner"}'),
            doc_change("PutSource", path="externals/inner/tsugumi.json", bytes=nested.encode()),
            doc_change("PutSource", path="externals/inner/definitions/inside/_card.json", bytes=b'{"template":"draft"}'),
            doc_change("SetLayout", json='{"cards":{"dropped":{"x":17,"y":29}}}')])
        assert result.rev == snapshot.rev + 1
        layout = json.loads(text(result.delta.layoutJson))
        assert set(layout["cards"]) == {"dropped", "automatic", "outer"}
        assert layout["cards"]["dropped"] == {"x": 17, "y": 29}
        assert "inside" in json.loads((project / "externals/inner/layout.json").read_text())["cards"]
        assert client.call("ProjectInspectRequest").rev == result.rev
        # A source-only save also materializes legacy nested places.
        (project / "externals/inner/layout.json").unlink()
        client.call("ProjectReloadRequest", baseRev=result.rev, opId=str(uuid.uuid4()), origin="user")
        snapshot = client.call("ProjectInspectRequest")
        result = save(host, daemon, client, snapshot, [doc_change("PutSource",
            path="definitions/automatic/note.txt", bytes=b"next edit")])
        assert result.rev == snapshot.rev + 1
        assert "inside" in json.loads((project / "externals/inner/layout.json").read_text())["cards"]
    finally:
        host.close()


def test_document_patch_adds_a_card_two_workflows_deep_in_one_save(core_factory, project, tmp_path):
    def graph(ids):
        return json.dumps({"name": "Nested", "pkgsPath": "tsugumi:default",
            "boundary": {"inputs": {}, "outputs": {}},
            "canvas": {"cards": [{"id": card} for card in ids], "connections": []}})

    for prefix, card, external in [("", "outer", "one"), ("externals/one/", "middle", "two")]:
        directory = project / prefix
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "tsugumi.json").write_text(graph([card]))
        definition = directory / "definitions" / card
        definition.mkdir(parents=True)
        (definition / "_link.json").write_text(json.dumps({"external": external}))
    prefix = "externals/one/externals/two/"
    deep = project / prefix
    definition = deep / "definitions/legacy"
    definition.mkdir(parents=True)
    (definition / "_card.json").write_text('{"template":"draft"}')
    before = graph(["legacy"])
    (deep / "tsugumi.json").write_text(before)
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    host = Host(tmp_path / "host")
    try:
        register(host, daemon)
        snapshot = client.call("ProjectInspectRequest")
        derived = display(host, before)
        assert not (deep / "layout.json").exists()
        source = next(s for s in snapshot.sources if text(s.path) == prefix + "tsugumi.json")
        from Tsugumi.RPC.SourcePatch import SourcePatchT
        patch = SourcePatchT()
        patch.offset = 0
        patch.remove = len(before.encode())
        patch.text = graph(["added", "legacy"])
        result = save(host, daemon, client, snapshot, [
            doc_change("PatchSource", path=prefix + "tsugumi.json", baseHash=source.hash, patches=[patch]),
            doc_change("PutSource", path=prefix + "definitions/added/_card.json", bytes=b'{"template":"draft"}')])
        assert result.rev == snapshot.rev + 1
        positions = json.loads((deep / "layout.json").read_text())["cards"]
        assert set(positions) == {"added", "legacy"}
        assert positions["legacy"] == derived["cards"]["legacy"]
        assert positions["added"] != positions["legacy"]
        assert "outer" in json.loads(text(result.delta.layoutJson))["cards"]
        assert "middle" in json.loads((project / "externals/one/layout.json").read_text())["cards"]
    finally:
        host.close()
