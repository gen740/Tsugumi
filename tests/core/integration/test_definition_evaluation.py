"""CORRECTNESS: changed derivation inputs must never run an old definition.

These use the embedded evaluator and real worker. They intentionally do not
require a particular number of evaluations or builds, including on reversion.
"""

import json
from pathlib import Path

import pytest

import flatbuffers

from harness.daemon import ROOT
from harness.definition_evaluation import (
    EMPTY_OUTPUTS, completed_definition, edit_json, reload_project, synthetic_run, synthetic_sources,
)
from harness.real_cards import nix_string
from harness.scenario import Project, run, wait_run
from projects import card, conn, write
from tsugumi_client import path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.PortKind import PortKind
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload

ARGUMENTS = "{ pkgs, params, inputs, schemas }"


def expression(value):
    return ARGUMENTS + ": { run = " + synthetic_run(value) + "; }"


def sources(entry, **extra):
    return {**synthetic_sources(), "default.nix": entry, **extra}


def schema_says(name, word, then, otherwise):
    """A value taken from a port schema file the core generated for the builder."""
    return (f'(if pkgs.lib.hasInfix "{word}" (builtins.readFile (schemas + "/{name}")) '
            f'then "{then}" else "{otherwise}")')


def first_table(value):
    builder = flatbuffers.Builder(64)
    string = builder.CreateString(value)
    builder.StartObject(1)
    builder.PrependUOffsetTRelativeSlot(0, string, 0)
    builder.Finish(builder.EndObject())
    return bytes(builder.Output())


def definition(**extra):
    return {"type": "code", "sandbox": {"filesystem": False}, "reuseWorker": True, **extra}


@pytest.mark.parametrize("changed_input", [
    "entry", "import", "read-file", "file-added", "file-removed", "nix-param",
    "port-schema", "output-schema",
])
def test_changed_input_replaces_the_recorded_definition(core_factory, project, changed_input):
    """An input edit changes output and identity, and drops the old resident."""
    settings = definition()
    outputs = EMPTY_OUTPUTS
    files = {"default.nix": expression('"first"')}
    changed_file, replacement = "default.nix", expression('"second"')
    first, second = "first\n", "second\n"
    if changed_input == "import":
        files = {"default.nix": expression("import ./message.nix"), "message.nix": '"first"'}
        changed_file, replacement = "message.nix", '"second"'
    elif changed_input == "read-file":
        files = {"default.nix": expression("builtins.readFile ./message.txt"), "message.txt": "first"}
        changed_file, replacement = "message.txt", "second"
    elif changed_input in ("file-added", "file-removed"):
        # Directory enumeration and pathExists observe additions/removals even
        # when no pre-existing file has changed.
        present = ('builtins.hasAttr "optional.txt" (builtins.readDir ./.)'
                   if changed_input == "file-added" else 'builtins.pathExists ./optional.txt')
        files = {"default.nix": expression(
            'if ' + present + ' then builtins.readFile ./optional.txt else "first"')}
        changed_file, replacement = "optional.txt", "second" if changed_input == "file-added" else None
        if changed_input == "file-removed":
            files[changed_file] = "second"
            first, second = second, first
    elif changed_input == "nix-param":
        settings["params"] = [{"name": "message", "type": "string", "nix": True, "data": "first"}]
        files = {"default.nix": expression("params.message")}
        changed_file = "_card.json"
        replacement = json.dumps({**settings, "params": [dict(settings["params"][0], data="second")]})
    elif changed_input == "port-schema":
        # A builder generates the Card's API from these exact schemas. Observe
        # them in the body as well so a stale directory cannot pass unnoticed.
        files = {"default.nix": expression(schema_says("_params.fbs", "second", "second", "first"))}
        settings["params"] = [{"name": "first", "type": "string", "data": "value"}]
        changed_file = "_card.json"
        replacement = json.dumps({**settings, "params": [dict(settings["params"][0], name="second")]})
    elif changed_input == "output-schema":
        files = {"default.nix": expression(
            schema_says("_outputs.fbs", "Second", "outputs.out.Second", "outputs.out.First"))}
        outputs = 'table First { value:string; } table Outputs { out:First; } root_type Outputs;'
        changed_file, replacement = "_outputs.fbs", outputs.replace("First", "Second")
        first, second = "outputs.out.First\n", "outputs.out.Second\n"
    client = core_factory().connect()
    Project(project).card("work", settings, outputs=outputs, sources={**synthetic_sources(), **files}).save(client)
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
    """A new valid pin changes packages and identity; a bad hash cannot reuse them."""
    client = core_factory().connect()
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS,
                          sources=sources(expression("pkgs.lib.version"))).save(client)
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
    assert before[0] != after[0]
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
    producer = (ARGUMENTS + ': { exports.message = "first";'
                ' exports.unused = throw "exports must be lazy"; run = ' + synthetic_run('"producer"') + '; }')
    value = "inputs.data.message" if dependency_input == "export" else (
        schema_says("_inputs.fbs", "Second", "inputs.data.Second", "inputs.data.First"))
    files = {f"definitions/{name}/{file}": content for name, outputs_ in
             (("producer", [("out", PortKind.FlatBuffer, first_table("token"))]), ("work", []))
             for file, content in synthetic_sources(outputs_).items()}
    write(project, {
        "producer": {**definition(), "_outputs": outputs, "_nix": producer},
        "work": {**definition(inputs=[{"name": "data"}]),
                 "_outputs": EMPTY_OUTPUTS, "_nix": expression(value)},
    }, [card("producer", "producer"), card("work", "work")], [conn("producer", "out", "work", "data")], files)
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


@pytest.mark.parametrize("list_run", [False, True])
def test_run_and_explicit_views_are_in_the_plan_closure(core_factory, project, list_run):
    """Keep declaration order and build references, including a standalone HTML file."""
    client = core_factory().connect()
    library = synthetic_run('"abi9"')
    # A passthru entry would fail if the evaluator tried to read or merge it.
    library = "(" + library + ').overrideAttrs (_: { passthru.views = throw "implicit views"; })'
    body = '[ { label = "first"; run = library; } library ]' if list_run else 'library'
    views = """[
      { name = "second"; type = "html"; blob = ./notes.html; }
      { name = "first"; type = "dawn"; blob = pkgs.writeText "view.wasm" "opaque"; }
    ]""" if list_run else '[ ]'
    entry = (ARGUMENTS + ": let library = " + library + "; in { run = " + body
             + "; views = " + views + "; }")
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources=sources(
        entry, **{"notes.html": "<!doctype html><p>View</p>"})).save(client)
    run_id = run(client)
    assert wait_run(client, run_id).state == RunState.succeeded
    environment, = client.call("RunEnvironmentsRequest", runId=run_id).environments
    plan_path = Path(text(environment.manifest))
    plan = json.loads(plan_path.read_text())
    assert set(plan) == {"runs", "views"}
    assert len(plan["runs"]) == (2 if list_run else 1)
    assert all((Path(package) / "lib/main.so").is_file() for package in plan["runs"])
    assert [(view["name"], view["type"]) for view in plan["views"]] == (
        [("second", "html"), ("first", "dawn")] if list_run else [])
    if list_run:
        assert Path(plan["views"][0]["blob"]).read_text() == "<!doctype html><p>View</p>"
        records = client.call("RunInvocationsRequest", runId=run_id, card=path("work")).records
        assert [text(row.implementationLabel) or "" for row in records] == ["first", ""]
    paths = client.call("RunEnvironmentPathsRequest", runId=run_id, environmentId=text(environment.id),
                        limit=500).paths
    assert {str(plan_path), *plan["runs"], *(view["blob"] for view in plan["views"])} <= set(map(text, paths))


def test_declared_views_are_available_without_a_run_build(core_factory, project):
    """Visible Cards publish their ordered declarations without evaluating run."""
    client = core_factory().connect()
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources={
        "default.nix": ARGUMENTS + """: {
          run = throw "run must stay lazy";
          views = [
            { name = "text"; type = "html"; blob = pkgs.writeText "text.html" "<p>text</p>"; }
            { name = "notes"; type = "html"; blob = ./notes.html; }
          ];
        }""",
        "notes.html": "<p>Available before a run</p>",
    }).save(client)
    opened = client.open()
    client.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("work")])
    update = client.wait_notification(
        lambda note: note.valueType == NotificationPayload.CardViewsUpdated
        and note.value.card.segments == [b"work"] and bool(note.value.views)
    ).value
    assert [text(view.name) for view in update.views] == ["text", "notes"]
    assert Path(text(update.views[-1].blob)).read_text() == "<p>Available before a run</p>"


@pytest.mark.parametrize("body, message", [
    ('run = "echo legacy";', "must be a derivation"),
    ('run = "legacy"; preRun = "true";', "unknown Card attributes: preRun"),
    ('run = "legacy"; postRun = "true";', "unknown Card attributes: postRun"),
    ('run = "legacy"; sdk = {};', "unknown Card attributes: sdk"),
    ('run = []; views = {};', "Card views must be a list"),
    ('run = []; views = [{ name = "x"; type = "js"; blob = ./default.nix; }];',
     "a Card view must declare"),
])
def test_legacy_definition_forms_are_rejected(core_factory, project, body, message):
    client = core_factory().connect()
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources={
        "default.nix": ARGUMENTS + ": { " + body + " }",
    }).save(client)
    status = wait_run(client, run(client))
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.nix_failed
    assert message in text(status.failure.diagnostic)


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
        header = ("printf '#define VALUE \"%s\"\\n' \"$(cat ${package}/value)\""
                  " > value.h")
        entry = (ARGUMENTS + ": let package = import ./package.nix inputs; in { run = "
                 + synthetic_run(header=header) + "; }")
        client = core_factory().connect()
        Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources=sources(
            entry, **{"package.nix": 'inputs: inputs.sources.${"fix" + "ture"}.packages.sample'}
        )).save(client)
        unused = dict(source, name="unused", ref=source["ref"].replace(
            "/flake.tar.gz?", "/missing.tar.gz?"))
        edit_json(project / "tsugumi.json", lambda doc: doc.update(nixSources=[unused, source]))
        reload_project(client)
        before = completed_definition(client)
        assert before[0] == "original\n"
        edit_json(project / "tsugumi.json", lambda doc: doc.update(nixSources=[unused, changed]))
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
                          sources=sources(expression("pkgs.stdenv.hostPlatform.system"))).save(client)
    before = completed_definition(client)
    assert before[0] == native + "\n"
    edit_json(project / "definitions/work/_card.json", lambda card: card.update(system=foreign))
    reload_project(client)
    after = completed_definition(client)
    assert after[0] == foreign + "\n" and after[1] != before[1]
    assert text(after[2].executionSystem) == foreign
    plan = json.loads(Path(after[1][1]).read_text())
    assert plan["worker"].endswith("/bin/tsugumi-worker")
    assert Path(plan["worker"]).is_file()
    assert after[2].workerReused is False


def test_process_environment_is_not_an_implicit_definition_input(core_factory, project, monkeypatch):
    """Restricted evaluation masks the process environment from getEnv."""
    write(project, {"work": {**definition(), "_outputs": EMPTY_OUTPUTS,
                            "_nix": expression('"masked:" + builtins.getEnv "TSUGUMI_EVALUATION_SECRET"')}},
          [card("work", "work")], [], {"definitions/work/" + name: content
                                       for name, content in synthetic_sources().items()})
    monkeypatch.setenv("TSUGUMI_EVALUATION_SECRET", "first")
    assert completed_definition(core_factory().connect())[0] == "masked:\n"


def test_impure_file_read_cannot_fall_back_to_an_old_definition(core_factory, project, tmp_path):
    """Restricted evaluation rejects a new absolute file input outside the confirmed sources."""
    client = core_factory().connect()
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS,
                          sources=sources(expression('"first"'))).save(client)
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


def card_api(client, card="work"):
    return client.call("CardApiRequest", card=path(card), atRev=client.call("ProjectInspectRequest").rev)


def api_files(implementation):
    return {text(f.path): bytes(f.content) for f in implementation.files}


def test_card_api_returns_what_the_builder_placed_from_the_current_wiring(core_factory, project):
    """The Card API builds the saved Card and reads share/tsugumi/api/ without interpreting it."""
    client = core_factory().connect()
    producer = 'table First { value:string; } table Outputs { out:First; } root_type Outputs;'
    graph = Project(project).card("producer", definition(), outputs=producer, sources=sources(
        expression('"producer"')))
    graph.card("work", definition(inputs=[{"name": "data"}]), outputs=EMPTY_OUTPUTS,
               sources=sources(expression('"work"')))
    graph.save(client)
    result = card_api(client)
    assert result.failure is None, text(result.failure.diagnostic)
    unconnected, = result.implementations
    files = api_files(unconnected)
    # Regular files only, nested folders included, symlinks not followed.
    assert sorted(files) == ["README.md", "_inputs.fbs", "_outputs.fbs", "_params.fbs", "include/card.h", "raw"]
    assert files["README.md"] == b"# Card API\n"
    assert files["raw"] == b"\x00\xff"
    assert b"First" not in files["_inputs.fbs"]
    assert (Path(text(unconnected.package)) / "lib/main.so").is_file()
    # A new wire changes the schemas the next build gets; nothing is cached.
    graph.connect("producer", "out", "work", "data").save(client)
    connected, = card_api(client).implementations
    assert b"First" in api_files(connected)["_inputs.fbs"]
    assert text(connected.package) != text(unconnected.package)


def test_card_api_lists_each_run_implementation_and_reports_a_failed_build(core_factory, project):
    """A run list gives one entry per implementation; a failed build gives its failure and log."""
    client = core_factory().connect()
    entry = (ARGUMENTS + ': { run = [ { label = "first"; run = ' + synthetic_run('"one"', name="first")
             + '; } ' + synthetic_run('"two"', name="second") + ' ]; }')
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources=sources(entry)).save(client)
    result = card_api(client)
    assert result.failure is None
    assert [(r.index, text(r.label) if r.label else None) for r in result.implementations] == [(1, "first"), (2, None)]
    assert all("README.md" in api_files(r) for r in result.implementations)

    # A symlink above the API directory may not publish files from another output.
    linked = (ARGUMENTS + ': let real = ' + synthetic_run('"one"', name="first")
              + """; in { run = pkgs.runCommand "linked-card" { } ''
                mkdir -p "$out/lib"
                cp ${real}/lib/main.so "$out/lib/"
                ln -s ${real}/share "$out/share"
              ''; }""")
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources=sources(linked)).save(client)
    result = card_api(client)
    assert result.failure is None
    implementation, = result.implementations
    assert not implementation.files

    broken = (ARGUMENTS + ': { run = pkgs.runCommand "broken-card" { } '
              "''\n echo deliberate-build-failure; exit 1\n''; }")
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources={"default.nix": broken}).save(client)
    failed = card_api(client)
    assert not failed.implementations
    assert failed.failure.kind == FailureKind.nix_failed
    assert "deliberate-build-failure" in text(failed.log)
