"""The host reads accepted requirements through the project RPC, not Card JSON."""
import json

from harness.document import card_document, card_readme, project_document
from projects import put
from tsugumi_client import change, new_id, text


def test_host_requirements_follow_acceptance_and_keep_card_identity(tmp_path, core_factory):
    root = tmp_path / "project"
    put(root / "tsugumi.json", json.dumps(project_document(
        hostPaths=[{"name": "data", "description": "Input data"}],
        canvas={"cards": [{"id": "a"}], "connections": []})))
    card = {"type": "code", "grants": {"hostPaths": [{"name": "data", "write": False}]}}
    put(root / "definitions/a/_card.json", json.dumps(card_document(card)))
    put(root / "definitions/a/README.md", card_readme("A"))
    client = core_factory(project_root=root, mocked_builds=True).connect()

    def requirements():
        result = client.call("ProjectHostRequirementsRequest")
        return result.rev, [([text(s) for s in f.card.segments], text(f.name), f.write)
                            for f in result.folders or []]

    revision, folders = requirements()
    assert folders == [(["a"], "data", False)]
    card["grants"]["hostPaths"][0]["write"] = True
    put(root / "definitions/a/_card.json", json.dumps(card_document(card)))
    assert requirements() == (revision, folders)
    client.call("ProjectReloadRequest", opId=new_id(), origin="test", baseRev=revision,
                approvedPermissions=["host_folder_write a data"])
    revision, folders = requirements()
    assert folders == [(["a"], "data", True)]
    client.call("ProjectSaveRequest", opId=new_id(), origin="test", baseRev=revision,
                changes=[change("PutDefinition", id="a", cardJson=json.dumps(card_document({"type": "code"})), keepReadme=True)])
    assert requirements()[1] == []
