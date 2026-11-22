"""The host reads accepted requirements through the project RPC, not Card JSON."""
import json

from projects import put
from tsugumi_client import change, new_id, text


def test_host_requirements_follow_acceptance_and_keep_card_identity(tmp_path, core_factory):
    root = tmp_path / "project"
    put(root / "tsugumi.json", json.dumps({
        "pkgsPath": "tsugumi:default",
        "canvas": {"cards": [{"id": "a"}], "connections": []},
    }))
    card = {"type": "code", "grants": {"hostPaths": [{"name": "data", "path": "/data"}]}}
    put(root / "definitions/a/_card.json", json.dumps(card))
    put(root / "definitions/unused/_card.json", json.dumps({
        "type": "code",
        "grants": {"hostPaths": [{"name": "ignored", "path": "/ignored", "write": True}]},
    }))
    client = core_factory(project_root=root, mocked_builds=True).connect()

    def requirements():
        result = client.call("ProjectHostRequirementsRequest")
        return result.rev, [([text(s) for s in f.card.segments], text(f.path), f.write)
                            for f in result.folders or []]

    revision, folders = requirements()
    assert folders == [(["a"], "/data", False)]
    card["grants"]["hostPaths"][0]["write"] = True
    put(root / "definitions/a/_card.json", json.dumps(card))
    assert requirements() == (revision, folders)
    client.call("ProjectReloadRequest", opId=new_id(), origin="test", baseRev=revision,
                approvedPermissions=["host_folder_write a /data"])
    revision, folders = requirements()
    assert folders == [(["a"], "/data", True)]
    client.call("ProjectSaveRequest", opId=new_id(), origin="test", baseRev=revision,
                changes=[change("PutDefinition", id="a", cardJson='{"type":"code"}')])
    assert requirements()[1] == []
