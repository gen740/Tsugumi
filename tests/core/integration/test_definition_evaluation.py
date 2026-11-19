"""CORRECTNESS: changed derivation inputs must never run an old definition.

These use the embedded evaluator and real worker. They intentionally do not
require a particular number of evaluations or builds, including on reversion.
"""

import json
import shutil
from pathlib import Path

import pytest

from harness.daemon import ROOT, SDK
from harness.definition_evaluation import (
    EMPTY_OUTPUTS, completed_definition, edit_json, reload_project,
)
from harness.real_cards import nix_string
from harness.scenario import Project, run, wait_run
from projects import card, conn, write
from tsugumi_client import path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState


def expression(value):
    return '{ pkgs, inputs, params }: { run = "echo ${' + value + '}"; }'


def definition(**extra):
    return {"language": "bash", "sandbox": False, "reuseWorker": True, **extra}


@pytest.mark.parametrize("changed_input", [
    "entry", "import", "read-file", "file-added", "file-removed", "nix-param",
    "generated-contract", "output-schema",
])
def test_changed_input_replaces_the_recorded_definition(core_factory, project, changed_input):
    """An input edit changes output and identity, and drops the old resident."""
    settings = definition()
    outputs = EMPTY_OUTPUTS
    sources = {"default.nix": expression('"first"')}
    changed_file, replacement = "default.nix", expression('"second"')
    first, second = "first\n", "second\n"
    if changed_input == "import":
        sources = {"default.nix": expression("import ./message.nix"), "message.nix": '"first"'}
        changed_file, replacement = "message.nix", '"second"'
    elif changed_input == "read-file":
        sources = {"default.nix": expression("builtins.readFile ./message.txt"), "message.txt": "first"}
        changed_file, replacement = "message.txt", "second"
    elif changed_input in ("file-added", "file-removed"):
        # Directory enumeration and pathExists observe additions/removals even
        # when no pre-existing file has changed.
        present = ('builtins.hasAttr "optional.txt" (builtins.readDir ./.)'
                   if changed_input == "file-added" else 'builtins.pathExists ./optional.txt')
        sources = {"default.nix": expression(
            'if ' + present + ' then builtins.readFile ./optional.txt else "first"')}
        changed_file, replacement = "optional.txt", "second" if changed_input == "file-added" else None
        if changed_input == "file-removed":
            sources[changed_file] = "second"
            first, second = second, first
    elif changed_input == "nix-param":
        settings["params"] = [{"name": "message", "type": "string", "nix": True, "data": "first"}]
        sources = {"default.nix": expression("params.message")}
        changed_file = "_card.json"
        replacement = json.dumps({**settings, "params": [dict(settings["params"][0], data="second")]})
    elif changed_input == "generated-contract":
        # SDK code generation consumes this exact contract. Observe it in the
        # body as well so a stale generated directory cannot pass unnoticed.
        sources = {"default.nix": expression(
            '(builtins.head (builtins.fromJSON (builtins.readFile '
            '(inputs.tsugumi.generated + "/_contract.json"))).params).name')}
        settings["params"] = [{"name": "first", "type": "string", "data": "value"}]
        changed_file = "_card.json"
        replacement = json.dumps({**settings, "params": [dict(settings["params"][0], name="second")]})
    elif changed_input == "output-schema":
        sources = {"default.nix": expression(
            '(builtins.head (builtins.fromJSON (builtins.readFile '
            '(inputs.tsugumi.generated + "/_contract.json"))).outputs).type')}
        outputs = 'table First { value:string; } table Outputs { out:First; } root_type Outputs;'
        changed_file, replacement = "_outputs.fbs", outputs.replace("First", "Second")
        first, second = "outputs.out.First\n", "outputs.out.Second\n"
    client = core_factory().connect()
    Project(project).card("work", settings, outputs=outputs, sources=sources).save(client)
    before = completed_definition(client)
    assert before[0] == first

    target = project / "definitions/work" / changed_file
    original = target.read_bytes() if changed_input == "entry" else None
    if replacement is None:
        target.unlink()
    else:
        target.write_text(replacement)
    reload_project(client)
    after = completed_definition(client)
    assert after[0] == second
    assert after[1] != before[1]
    assert after[2].workerReused is False
    if original is not None:
        target.write_bytes(original)
        reload_project(client)
        assert completed_definition(client)[:2] == before[:2]


# A nixos-26.05 revision resolved once while validating this fixture. Keep the
# revision/hash fixed at runtime. nixpkgs-23.11's esbuild lacks LC_UUID and
# cannot run on current macOS, so it cannot exercise a successful pin switch.
SECOND_PIN = ("https://github.com/NixOS/nixpkgs/archive/"
              "f1e5c356f35d07e7b71a6b5e6c9338e0b029e0c0.tar.gz#"
              "sha256-qXFIqA6FCcx7XeDzw0ZNlkDT9UiCzrNDwK8RuU8RcHM=")


def test_nixpkgs_revision_and_hash_select_the_next_definition(core_factory, project):
    """A new valid pin changes packages, shell, PATH and identity; a bad hash cannot reuse them."""
    client = core_factory().connect()
    # Read nixpkgs' version and the actual shell/PATH, not the project setting
    # echoed back by the fixture. The manifest must select all three together.
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS,
                          sources={"default.nix": '''{ pkgs, inputs, params }: { run = ''
                            echo ${pkgs.lib.version}
                            printf '%s\\n' "$BASH" "$PATH"
                          ''; }'''}).save(client)
    project_file = project / "tsugumi.json"
    locked = json.loads((ROOT / "flake.lock").read_text())["nodes"]["nixpkgs"]["locked"]
    primary_pin = ("https://github.com/NixOS/nixpkgs/archive/" + locked["rev"]
                   + ".tar.gz#" + locked["narHash"])
    edit_json(project_file, lambda doc: doc.update(pkgsPath=primary_pin))
    reload_project(client)
    before = completed_definition(client)
    edit_json(project_file, lambda doc: doc.update(pkgsPath=SECOND_PIN))
    reload_project(client)
    after = completed_definition(client)
    old_tools, new_tools = before[0].splitlines(), after[0].splitlines()
    assert len(old_tools) == len(new_tools) == 3
    assert all(old != new for old, new in zip(old_tools, new_tools)), (old_tools, new_tools)
    assert after[1] != before[1]
    assert after[2].workerReused is False

    # Same URL, different hash: neither pkgs_for's cache nor a future full
    # definition cache may hide a mismatching fixed-output hash.
    edit_json(project_file, lambda doc: doc.update(
        pkgsPath=SECOND_PIN.split("#")[0] + "#sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="))
    reload_project(client)
    failed = run(client)
    status = wait_run(client, failed)
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.nix_failed
    assert not text(client.call("RunRecordRequest", runId=failed, card=path("work")).record.summary.environmentId)


@pytest.mark.parametrize("dependency_input", ["export", "schema"])
def test_connected_input_change_replaces_downstream_definition(core_factory, project, dependency_input):
    """A producer's export or type is an input even with unchanged consumer files."""
    outputs = 'table First { value:string; } table Outputs { out:First; } root_type Outputs;'
    script = '''printf '%s' '{"out":{"value":"token"}}' > "$TSUGUMI_OUTPUT"'''
    producer = ('{ pkgs, inputs, params }: { exports.message = "first"; run = '
                + nix_string(script) + '; }')
    value = "inputs.data.message" if dependency_input == "export" else (
        '(builtins.head (builtins.fromJSON (builtins.readFile '
        '(inputs.tsugumi.generated + "/_contract.json"))).inputs).type')
    write(project, {
        "producer": {**definition(), "_outputs": outputs, "_nix": producer},
        "work": {**definition(inputs=[{"name": "data"}]),
                 "_outputs": EMPTY_OUTPUTS, "_nix": expression(value)},
    }, [card("producer", "producer"), card("work", "work")], [conn("producer", "out", "work", "data")])
    client = core_factory().connect()
    before = completed_definition(client, start="producer")
    target = project / "definitions/producer" / ("default.nix" if dependency_input == "export" else "_outputs.fbs")
    original = target.read_text()
    target.write_text(original.replace('"first"', '"second"') if dependency_input == "export"
                      else original.replace("First", "Second"))
    reload_project(client)
    after = completed_definition(client, start="producer")
    expected = ("first\n", "second\n") if dependency_input == "export" else (
        "inputs.data.First\n", "inputs.data.Second\n")
    assert (before[0], after[0]) == expected
    assert after[1] != before[1]
    assert after[2].workerReused is False


@pytest.mark.parametrize(("sdk_input", "restart"), [
    pytest.param("nix/default.nix", False, id="resident"),
    pytest.param("views/line-chart.js", True, id="restart"),
])
def test_sdk_deployment_changes_the_next_definition(core_factory, project, tmp_path, sdk_input, restart):
    """Changed SDK contents at the same path replace the definition, with or without restart."""
    sdk = tmp_path / "sdk"
    shutil.copytree(SDK, sdk)
    write(project, {"work": {**definition(), "_outputs": EMPTY_OUTPUTS,
                            "_nix": expression('"sdk"')}}, [card("work", "work")], [])
    daemon = core_factory(args=["--sdk-dir", str(sdk)])
    client = daemon.connect()
    before = completed_definition(client)
    if restart:
        daemon.stop()
    target = sdk / sdk_input
    # Installed Nix SDK files are read-only; edit only this private copy.
    target.chmod(target.stat().st_mode | 0o200)
    if sdk_input == "nix/default.nix":
        upstream = target.read_text()
        target.write_text("args:\nlet sdk = (\n" + upstream + r'''
) args;
in sdk // {
  manifestFor = remote: card: sdk.manifestFor remote (card // {
    run = card.run + "\necho sdk-updated";
  });
}
''')
    else:
        with (sdk / sdk_input).open("a") as file:
            file.write('\nexport const evaluationRevision = "updated";\n')
    if restart:
        daemon.start()
        client = daemon.connect()
    after = completed_definition(client)
    observed = json.dumps({"sdk_input": sdk_input, "before_stdout": before[0], "after_stdout": after[0],
                           "before_environment": before[1], "after_environment": after[1],
                           "worker_reused": after[2].workerReused})
    if sdk_input == "nix/default.nix":
        assert before[0] == "sdk\n" and after[0] == "sdk\nsdk-updated\n", observed
    else:
        # This SDK file reaches the manifest via the bundled view, not stdout.
        # Inspect the actual built artifact in the recorded manifest.
        environment, = client.call("RunEnvironmentsRequest", runId=text(after[2].runId)).environments
        manifest = json.loads(Path(text(environment.manifest)).read_text())
        views = json.loads(manifest["views"])
        updated_bundle = "updated" in (Path(views["line-chart"]) / "index.js").read_text()
        assert updated_bundle, observed
    assert after[1] != before[1], observed
    assert after[2].workerReused is False, observed


def test_project_flake_change_replaces_the_definition(core_factory, project, tmp_path, monkeypatch):
    """A new opted-in flake cannot reuse the earlier package or resident."""
    from contextlib import ExitStack
    from harness.nix_source import source_fixture

    with ExitStack() as stack:
        first_dir = tmp_path / "first"
        first_dir.mkdir()
        source, _ = stack.enter_context(source_fixture(first_dir, monkeypatch))
        second_dir = tmp_path / "second"
        second_dir.mkdir()
        changed, _ = stack.enter_context(source_fixture(second_dir, monkeypatch, message="second"))
        write(project, {"work": {**definition(), "_outputs": EMPTY_OUTPUTS,
                                "_nix": '{ pkgs, inputs, params }: { run = '
                                '"cat ${inputs.sources.fixture.packages.sample}/value"; }'}},
              [card("work", "work")], [])
        edit_json(project / "tsugumi.json", lambda doc: doc.update(nixSources=[source]))
        client = core_factory().connect()
        before = completed_definition(client)
        assert before[0] == "original\n"
        edit_json(project / "tsugumi.json", lambda doc: doc.update(nixSources=[changed]))
        reload_project(client)
        after = completed_definition(client)
        assert after[0] == "second\n" and after[1] != before[1]
        assert after[2].workerReused is False


def test_target_system_change_selects_new_packages_and_definition(core_factory, project):
    """Changing an available execution system changes the package set and recorded identity."""
    client = core_factory().connect()
    service, = client.call("ProjectInspectRequest").executionServices
    native = text(service.nativeSystem)
    foreign = next((text(s) for s in service.systems if text(s) != native), None)
    if foreign is None:
        pytest.skip("requires a second local system with binfmt_misc and Nix extra-platforms (shared VM)")
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS,
                          sources={"default.nix": expression("pkgs.stdenv.hostPlatform.system")}).save(client)
    before = completed_definition(client)
    assert before[0] == native + "\n"
    edit_json(project / "definitions/work/_card.json", lambda card: card.update(system=foreign))
    reload_project(client)
    after = completed_definition(client)
    assert after[0] == foreign + "\n" and after[1] != before[1]
    assert text(after[2].executionSystem) == foreign
    assert after[2].workerReused is False


def test_process_environment_is_not_an_implicit_definition_input(core_factory, project, monkeypatch):
    """Restricted evaluation masks the process environment from getEnv."""
    write(project, {"work": {**definition(), "_outputs": EMPTY_OUTPUTS,
                            "_nix": expression('"masked:" + builtins.getEnv "TSUGUMI_EVALUATION_SECRET"')}},
          [card("work", "work")], [])
    monkeypatch.setenv("TSUGUMI_EVALUATION_SECRET", "first")
    assert completed_definition(core_factory().connect())[0] == "masked:\n"


def test_impure_file_read_cannot_fall_back_to_an_old_definition(core_factory, project, tmp_path):
    """Restricted evaluation rejects a new absolute file input outside the confirmed sources."""
    client = core_factory().connect()
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS,
                          sources={"default.nix": expression('"first"')}).save(client)
    assert completed_definition(client)[0] == "first\n"
    outside = tmp_path / "outside.txt"
    outside.write_text("must not reach Nix")
    (project / "definitions/work/default.nix").write_text(expression(
        "builtins.readFile (/. + " + nix_string(str(outside.resolve())) + ")"))
    reload_project(client)
    run_id = run(client)
    status = wait_run(client, run_id)
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.nix_failed
    record = client.call("RunRecordRequest", runId=run_id, card=path("work")).record
    assert not text(record.summary.environmentId)
