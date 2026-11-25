"""Validate the native export manifest and its preflight failures."""

import json
from pathlib import Path

import pytest

from harness.document import NATIVE_SYSTEM, card_document, card_readme, project_document
from harness.fixtures import mock_plan
from harness.scenario import STRING_OUTPUT


TARGET = "aarch64-darwin"


def write_project(root, declarations, host_path):
    root.mkdir(parents=True, exist_ok=True)
    (root / "tsugumi.json").write_text(json.dumps(project_document(
        canvas={"cards": [{"id": name} for name in declarations], "connections": []},
        hostPaths=[{"name": host_path, "description": "Exported app data"}],
    )))
    for name, definition in declarations.items():
        directory = root / "definitions" / name
        directory.mkdir(parents=True)
        (directory / "_card.json").write_text(json.dumps(card_document(definition)))
        (directory / "README.md").write_text(card_readme(name))
        if definition["type"] == "code":
            (directory / "default.nix").write_text("{ params, inputs }: { }")
            (directory / "_outputs.fbs").write_text(STRING_OUTPUT)


def export(daemon, manifest):
    daemon.stop()
    daemon.args = ["--prepare-native-cards", str(manifest)]
    daemon.start(expect_exit=True)
    return daemon.proc.returncode, daemon.diagnostics()


def test_rejects_unsupported_nested_card_and_invalid_graph(project, core_factory, tmp_path):
    write_project(project, {"sub": {"type": "subgraph"}}, "root_data")
    child = project / "externals" / "sub"
    write_project(child, {
        "work": {"type": "code", "systems": ["aarch64-linux"]},
        "unfinished": {"type": "draft"},
    }, "child_data")
    daemon = core_factory()
    manifest = tmp_path / "cards.json"

    result, diagnostics = export(daemon, manifest)

    assert result == 1, diagnostics
    assert "Card sub/work must declare aarch64-darwin in systems" in diagnostics
    assert "Card sub/unfinished: draft cards are not implemented" in diagnostics
    assert not manifest.exists()


def test_manifest_lists_declared_host_paths_without_grants(project, core_factory, tmp_path):
    write_project(project, {"sub": {"type": "subgraph"}}, "root_data")
    write_project(project / "externals" / "sub", {}, "child_data")
    daemon = core_factory()
    manifest = tmp_path / "cards.json"

    result, diagnostics = export(daemon, manifest)

    assert result == 0, diagnostics
    assert json.loads(manifest.read_text()) == {
        "version": 1, "system": TARGET, "host_paths": ["child_data", "root_data"], "cards": [],
    }


@pytest.mark.skipif(NATIVE_SYSTEM != TARGET, reason="requires an Apple Silicon native build target")
@pytest.mark.parametrize("build_error", [False, True])
def test_native_manifest_contains_resolved_leaves(project, core_factory, tmp_path, build_error):
    write_project(project, {"sub": {"type": "subgraph"}}, "root_data")
    child = project / "externals" / "sub"
    write_project(child, {
        "work": {"type": "code", "systems": ["aarch64-linux", TARGET], "reuseWorker": True},
    }, "child_data")
    definition = child / "definitions" / "work"
    (definition / "go.mod").write_text("module example.test/work\n")
    options = {"error": "mock failed"} if build_error else {}
    package = str(tmp_path / "package")
    (definition / "mock-build.json").write_text(json.dumps(mock_plan([package], **options)))
    daemon = core_factory(mocked_builds=True)
    manifest = tmp_path / "cards.json"

    result, diagnostics = export(daemon, manifest)

    if build_error:
        assert result == 1, diagnostics
        assert "Card sub/work:" in diagnostics
        assert "mock failed" in diagnostics
        assert not manifest.exists()
        return
    assert result == 0, diagnostics
    exported = json.loads(manifest.read_text())
    assert exported["version"] == 1 and exported["system"] == TARGET
    assert exported["host_paths"] == ["child_data", "root_data"]
    card, = exported["cards"]
    assert card["card"] == "sub/work"
    assert card["system"] == TARGET and card["reuse_worker"] is True
    assert card["language"] == "go"
    assert len(card["implementation_labels"]) == 1 and card["execution_list"] is False
    assert card["plan"] in card["closure"]
    assert all(Path(path).is_absolute() for path in card["closure"])
    assert json.loads(Path(card["plan"]).read_text())["runs"] == [package]
