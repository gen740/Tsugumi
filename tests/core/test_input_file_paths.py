"""File input paths have explicit Card-local and owning-project bases."""

import json

import pytest

from projects import FILE_FBS, card, put, run_and_wait, write
from tsugumi_client import path, text
from Tsugumi.Data.RunState import RunState
from Tsugumi.Data.FailureKind import FailureKind


def project_with_file(root, value, external=False):
    owner = root / "externals/child" if external else root
    write(owner,
          defs={"load": {"template": "input",
                         "params": [{"name": "value", "type": "string", "data": value}],
                         "retainArtifact": True, "artifactOutput": "doc", "_outputs": FILE_FBS},
                "shared": {"template": "trigger"}},
          cards=[card("load", "load"), card("shared", "shared")], conns=[],
          files={"definitions/load/data/events.txt": "Card-local contents",
                 "definitions/shared/data/events.txt": "Owning-project contents",
                 "data/events.txt": "Unlisted root file"})
    if external:
        doc = json.loads((owner / "tsugumi.json").read_text())
        doc["boundary"] = {"inputs": {}, "outputs": {"doc": {"card": "load", "port": "doc"}}}
        put(owner / "tsugumi.json", json.dumps(doc))
        write(root, defs={"shared": {"template": "trigger"}}, cards=[card("child"), card("shared", "shared")], conns=[],
              files={"definitions/shared/data/events.txt": "Outer-project contents"})
        (root / "definitions/child/_card.json").unlink()
        put(root / "definitions/child/_link.json", '{"external":"child"}')
    return owner, ["child", "load"] if external else ["load"]


def artifact_bytes(client, run_id, segments):
    artifacts = client.call("ArtifactListRequest", runId=run_id, card=path(*segments)).artifacts
    assert len(artifacts) == 1
    return bytes(client.call("ArtifactReadRequest", artifactId=text(artifacts[0].artifactId), limitBytes=4096).bytes)


@pytest.mark.parametrize("external", [False, True])
@pytest.mark.parametrize("value,expected", [("data/events.txt", b"Card-local contents"),
                                            ("project:definitions/shared/data/events.txt", b"Owning-project contents")])
def test_input_paths_choose_the_declared_base_and_save_the_file(project, core_factory, external, value, expected):
    """Local and project paths select the same base in root and external projects."""
    owner, segments = project_with_file(project, value, external)
    c = core_factory().connect()
    rid, status = run_and_wait(c, segments, timeout=10)
    assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
    assert artifact_bytes(c, rid, segments) == expected
    # The saved run owns a copy, even when both candidate source files change.
    (owner / "definitions/load/data/events.txt").write_text("changed local")
    (owner / "definitions/shared/data/events.txt").write_text("changed project")
    assert artifact_bytes(c, rid, segments) == expected


@pytest.mark.parametrize("external", [False, True])
@pytest.mark.parametrize("value", ["../data/events.txt", "/etc/passwd", "project:../data/events.txt",
                                   "project:/etc/passwd", "", "definitions/load/data/events.txt"])
def test_input_paths_reject_escape_and_implicit_project_paths(project, core_factory, external, value):
    """A File input cannot escape its declared base or silently use the old project base."""
    _, segments = project_with_file(project, value, external)
    c = core_factory().connect()
    _, status = run_and_wait(c, segments, timeout=10)
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.invalid_path
    assert "project:<path>" in text(status.failure.diagnostic)


@pytest.mark.parametrize("external", [False, True])
def test_missing_card_local_file_does_not_fall_back_to_project(project, core_factory, external):
    """An existing project file cannot change the meaning of a Card-local path."""
    owner, segments = project_with_file(project, "data/events.txt", external)
    (owner / "definitions/load/data/events.txt").unlink()
    c = core_factory().connect()
    _, status = run_and_wait(c, segments, timeout=10)
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.invalid_path


def test_input_file_must_be_in_the_project_source_manifest(project, core_factory, tmp_path):
    """A symlink to an outside file is not a saved project source."""
    owner, segments = project_with_file(project, "data/events.txt")
    outside = tmp_path / "outside.txt"
    outside.write_text("outside data")
    local = owner / "definitions/load/data/events.txt"
    local.unlink()
    local.symlink_to(outside)
    with pytest.raises(RuntimeError, match="symlink; links are not allowed"):
        core_factory()
