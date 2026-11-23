"""Input Artifact paths and IDs come from the immutable build output."""

import pytest

from harness.fixtures import mock_plan
from harness.scenario import Project, run, wait_run
from tsugumi_client import text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


OUTPUTS = """
include "tsugumi/artifact.fbs";
table File { payload:tsugumi.Artifact; }
table Outputs { out:File (required); }
root_type Outputs;
"""
ARTIFACT_ID = "00" * 16


@pytest.mark.parametrize("sidecar,folder", [
    (ARTIFACT_ID, "directory"), (ARTIFACT_ID, "symlink"), (ARTIFACT_ID, "missing"),
    ("../not-an-id", "missing"), ("A" * 32, "missing"),
])
def test_input_artifacts_require_binary_ids_and_contained_directories(
        core_factory, project, tmp_path, sidecar, folder):
    store = tmp_path / "store"
    package = store / "source"
    prefix = package / "share/tsugumi/input"
    ports = prefix / "ports"
    ports.mkdir(parents=True)
    # The bytes stay opaque to the core, including when a sidecar is present.
    (ports / "out").write_bytes(b"opaque")
    (ports / "out.artifacts").write_text(sidecar + "\n")
    artifact = prefix / "artifacts" / ARTIFACT_ID
    if folder == "directory":
        artifact.mkdir(parents=True)
        (artifact / "payload.txt").write_text("built file")
    elif folder == "symlink":
        artifact.parent.mkdir()
        artifact.symlink_to(tmp_path)
    client = core_factory(mocked_builds=True, env={"TSUGUMI_MOCK_STORE": str(store)}).connect()
    Project(project).card("work", {"type": "input"}, outputs=OUTPUTS,
                          plan=mock_plan([str(package)])).save(client)
    status = wait_run(client, run(client, "work", propagate=False))
    if folder == "directory":
        assert status.state == RunState.succeeded
    else:
        assert status.state == RunState.failed
        assert status.failure.kind == FailureKind.output_invalid
        assert "artifact" in text(status.failure.diagnostic).lower()
