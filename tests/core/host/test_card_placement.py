"""The host fills absent Card positions after writers, reloads and opening."""
import json
import time
import uuid

from harness.host import Host
from tsugumi_client import change as doc_change, text


def register(host, daemon):
    host.call("RegisterProjectRequest", projectId=daemon.ready["project_id"],
              endpoint="unix:" + daemon.unix, token=daemon.ready["token"],
              path=str(daemon.project))


def placed(client, ids):
    until = time.monotonic() + 5
    while True:
        snapshot = client.call("ProjectInspectRequest")
        layout = json.loads(text(snapshot.layoutJson))
        if all(card in layout.get("cards", {}) for card in ids):
            return layout
        assert time.monotonic() < until, f"host did not place {ids}: {layout}"
        time.sleep(0.01)


def test_host_places_external_saves_and_reload_without_moving_drop(core_factory, project, tmp_path):
    """CLI saves and imported Git edits get positions without any UI/Agent save."""
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    host = Host(tmp_path / "host")
    try:
        register(host, daemon)
        snapshot = client.call("ProjectInspectRequest")
        project_json = json.loads(text(snapshot.projectJson))
        project_json["canvas"]["cards"] = [{"id": "dropped"}, {"id": "unplaced"}]
        client.call("ProjectSaveRequest", baseRev=snapshot.rev, opId=str(uuid.uuid4()), origin="user",
                    changes=[doc_change("SetProject", json=json.dumps(project_json)),
                             doc_change("PutDefinition", id="dropped", cardJson='{"template":"draft"}'),
                             doc_change("PutDefinition", id="unplaced", cardJson='{"template":"draft"}'),
                             doc_change("SetLayout", json='{"cards":{"dropped":{"x":17,"y":29}}}')])
        first = placed(client, ["dropped", "unplaced"])
        assert first["cards"]["dropped"] == {"x": 17, "y": 29}
        assert first["cards"]["unplaced"] != first["cards"]["dropped"]
        # The same on-disk document change produced by checkout/pull plus reload.
        project_json["canvas"]["cards"].append({"id": "from-git"})
        (project / "tsugumi.json").write_text(json.dumps(project_json))
        definition = project / "definitions/from-git"
        definition.mkdir(parents=True)
        (definition / "_card.json").write_text('{"template":"draft"}')
        snapshot = client.call("ProjectInspectRequest")
        client.call("ProjectReloadRequest", baseRev=snapshot.rev, opId=str(uuid.uuid4()), origin="user")
        second = placed(client, ["dropped", "unplaced", "from-git"])
        assert second["cards"]["dropped"] == first["cards"]["dropped"]
        assert second["cards"]["unplaced"] == first["cards"]["unplaced"]
    finally:
        host.close()


def test_opening_a_workflow_places_its_missing_cards(core_factory, project, tmp_path):
    """A project first registered with missing layout needs no edit to be placed."""
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
        register(host, daemon)
        assert "opened" in placed(client, ["opened"])["cards"]
    finally:
        host.close()


def test_opening_places_cards_inside_a_card_in_card(core_factory, project, tmp_path):
    """Nested workflows own their layout and get positions on host registration."""
    graph = json.loads((project / "tsugumi.json").read_text())
    graph["canvas"]["cards"] = [{"id": "outer"}]
    (project / "tsugumi.json").write_text(json.dumps(graph))
    outer = project / "definitions/outer"
    outer.mkdir(parents=True)
    (outer / "_link.json").write_text('{"external":"inner"}')
    inner = project / "externals/inner"
    definition = inner / "definitions/inside"
    definition.mkdir(parents=True)
    (definition / "_card.json").write_text('{"template":"draft"}')
    (inner / "tsugumi.json").write_text(json.dumps({"name": "Inner", "pkgsPath": "tsugumi:default",
        "boundary": {"inputs": {}, "outputs": {}}, "canvas": {"cards": [{"id": "inside"}], "connections": []}}))
    daemon = core_factory(mocked_builds=True)
    client = daemon.connect()
    host = Host(tmp_path / "host")
    try:
        register(host, daemon)
        placed(client, ["outer"])
        until = time.monotonic() + 5
        while True:
            try:
                layout = json.loads((inner / "layout.json").read_text())
                if "inside" in layout.get("cards", {}):
                    break
            except FileNotFoundError:
                pass
            assert time.monotonic() < until, "nested workflow was not placed"
            time.sleep(0.01)
    finally:
        host.close()
