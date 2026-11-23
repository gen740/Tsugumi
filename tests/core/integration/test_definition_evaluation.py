"""CORRECTNESS: changed derivation inputs must never run an old definition.

These use the embedded evaluator and real worker. They intentionally do not
require a particular number of evaluations or builds, including on reversion.
"""

import json
from pathlib import Path
from urllib.parse import unquote, urlsplit

import pytest

import flatbuffers

from harness.definition_evaluation import (
    EMPTY_OUTPUTS, completed_definition, edit_json, reload_project, synthetic_run, synthetic_sources,
)
from harness.nix_path import test_lock_entries as fixture_locks, write_lock
from harness.real_cards import nix_string
from harness.scenario import Project, run, wait_run
from projects import card, card_in_card, conn, write_nix_project as write
from tsugumi_client import change, new_id, path, text
from Tsugumi.Data.FailureKind import FailureKind
from Tsugumi.Data.RunState import RunState
from Tsugumi.RPC.NotificationPayload import NotificationPayload

ARGUMENTS = "{ params, inputs }: let pkgs = import <nixpkgs> { }; in "


def expression(value):
    return ARGUMENTS + "{ run = " + synthetic_run(value) + "; }"


def sources(entry, **extra):
    return {**synthetic_sources(), "default.nix": entry, **extra}


def schema_says(path, word, then, otherwise):
    """A value taken from a port schema file the core generated for the builder."""
    return (f'(if pkgs.lib.hasInfix "{word}" (builtins.readFile ({path})) '
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
    "params-declaration", "output-schema",
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
        files = {"default.nix": expression("params.message"),
                 "params.nix": '{ message = "first"; }'}
        changed_file = "params.nix"
        replacement = '{ message = "second"; }'
    elif changed_input == "params-declaration":
        files = {"default.nix": expression(schema_says("./params.nix", "second", "second", "first")),
                 "params.nix": '{ first = "value"; }'}
        changed_file, replacement = "params.nix", '{ second = "value"; }'
    elif changed_input == "output-schema":
        files = {"default.nix": expression(
            schema_says("./_outputs.fbs", "Second", "outputs.out.Second", "outputs.out.First"))}
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


@pytest.mark.parametrize("case, diagnostic", [
    ("missing", 'nixPath "nixpkgs" is not locked; update the lock'),
    ("changed", 'nixPath "nixpkgs" changed source (lock has github:NixOS/nixpkgs/'),
])
def test_project_requires_a_lock_for_each_declared_name(core_factory, project, case, diagnostic):
    entries = fixture_locks()
    if case == "missing":
        write_lock(project, [entry for entry in entries if entry["name"] != "nixpkgs"])
    else:
        edit_json(project / "tsugumi.json", lambda doc: doc["nixPath"][0].update(url="github:other/nixpkgs"))
    client = core_factory().connect()
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources=sources(
        expression('"unused"'))).save(client)
    status = wait_run(client, run(client))
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.nix_failed
    assert diagnostic in text(status.failure.diagnostic)
    assert "update the lock" in text(status.failure.diagnostic)


def test_explicit_lock_returns_a_pinned_url_and_it_resolves_same_source_refs(core_factory, project):
    target = project / "tsugumi.lock"
    pin = next(entry for entry in fixture_locks() if entry["name"] == "nixpkgs")
    expected = urlsplit(pin["locked_url"])
    target.write_text('{"nixPath":{}}')
    edit_json(project / "tsugumi.json", lambda doc: doc.update(nixPath=[
        {"name": "nixpkgs", "url": "github:NixOS/nixpkgs/some-other-ref"},
    ]))
    client = core_factory().connect()
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources=sources(
        expression('"from locked source"'))).save(client)
    snapshot = client.call("ProjectInspectRequest")
    result = client.call("NixPathLockSourceRequest", url=f"{expected.scheme}:{expected.path}")
    locked_url = text(result.url)
    actual = urlsplit(locked_url)
    assert (actual.scheme, actual.path) == (expected.scheme, expected.path)
    query = {k: unquote(v) for k, v in (part.split("=", 1) for part in actual.query.split("&"))}
    assert query["narHash"] == expected.query.removeprefix("narHash=")
    assert query.keys() <= {"narHash", "lastModified"}
    assert json.loads(target.read_text()) == {"nixPath": {}}

    locked = json.dumps({"nixPath": {"nixpkgs": locked_url}})
    old = next(info for info in snapshot.sources if info.path == b"tsugumi.lock")
    client.call("ProjectSaveRequest", baseRev=snapshot.rev, opId=new_id(), origin="ui:test",
                changes=[change("SetProjectLock", path="tsugumi.lock", json=locked, baseHash=text(old.hash))])
    assert json.loads(target.read_text()) == json.loads(locked)
    evaluated = client.call("EvaluateNixRequest", card=path("work"),
                            expression="builtins.baseNameOf <nixpkgs/flake.nix>")
    assert evaluated.failure is None, evaluated.failure and text(evaluated.failure.diagnostic)
    assert json.loads(text(evaluated.json)) == "flake.nix"


def test_kit_derivation_is_independent_of_unused_parameter_values(core_factory, project):
    client = core_factory().connect()
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources={
        "params.nix": "{ unused = 7; }",
        "default.nix": ARGUMENTS + """
          let sdk = import <tsugumi-kit/sdk/python> { inherit pkgs; };
          in {
            run = sdk.pythonCard {
              inherit params inputs;
              outputs = ./_outputs.fbs;
              src = ./src;
            };
          }
        """,
        "src/main.py": "",
    }).save(client)

    def drv():
        result = client.call("EvaluateNixRequest", card=path("work"),
                             expression="(import ./. { inherit params inputs; }).run.drvPath")
        assert result.failure is None, result.failure and text(result.failure.diagnostic)
        return json.loads(text(result.json))

    before = drv()
    (project / "definitions/work/params.nix").write_text("{ unused = 42; }")
    reload_project(client)
    assert drv() == before


def test_undeclared_search_path_reports_the_project_names(core_factory, project, monkeypatch):
    monkeypatch.setenv("NIX_PATH", f"missing={project}")
    client = core_factory().connect()
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources=sources(
        expression("toString <missing>"))).save(client)
    status = wait_run(client, run(client))
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.nix_failed
    assert "undeclared nixPath name 'missing' (declared: nixpkgs, tsugumi-kit)" in text(status.failure.diagnostic)


@pytest.mark.parametrize("dependency_input", ["export", "schema"])
def test_connected_input_change_replaces_downstream_definition(core_factory, project, dependency_input):
    """A producer's export or type is an input even with unchanged consumer files."""
    outputs = 'table First { value:string; } table Outputs { out:First; } root_type Outputs;'
    producer = (ARGUMENTS + '{ exports.message = "first";'
                ' exports.unused = throw "exports must be lazy"; run = ' + synthetic_run('"producer"') + '; }')
    value = "inputs.data.exports.message" if dependency_input == "export" else (
        schema_says("inputs.data.type", "Second", "inputs.data.Second", "inputs.data.First"))
    files = {f"definitions/{name}/{file}": content for name, outputs_ in
             (("producer", [("out", first_table("token"))]), ("work", []))
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
    library = synthetic_run('"views"')
    # A passthru entry would fail if the evaluator tried to read or merge it.
    library = "(" + library + ').overrideAttrs (_: { passthru.views = throw "implicit views"; })'
    body = '[ { label = "first"; run = library; } library ]' if list_run else 'library'
    views = """[
      { name = "second"; type = "html"; blob = ./notes.html; }
      { name = "first"; type = "dawn"; blob = pkgs.writeText "view.wasm" "opaque"; }
    ]""" if list_run else '[ ]'
    entry = (ARGUMENTS + "let library = " + library + "; in { run = " + body
             + '; api = throw "api must stay lazy"; views = ' + views + "; }")
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
        "default.nix": ARGUMENTS + """{
          run = throw "run must stay lazy";
          views = [
            { name = "scene"; type = "dawn"; blob = pkgs.writeText "scene.wasm" "opaque"; }
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
    assert [(text(view.name), text(view.type)) for view in update.views] == [("scene", "dawn"), ("notes", "html")]
    assert Path(text(update.views[0].blob)).read_text() == "opaque"
    assert Path(text(update.views[1].blob)).read_text() == "<p>Available before a run</p>"


def test_a_failed_view_evaluation_is_relayed_apart_from_an_empty_list(core_factory, project):
    """A broken declaration says why; a Card that declares nothing sends no failure."""
    client = core_factory().connect()
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources={
        "default.nix": ARGUMENTS + '{ run = []; views = throw "views are broken"; }',
    }).save(client)
    opened = client.open()
    client.call("ViewSetVisibleRequest", subscriptionId=text(opened.subscriptionId), cards=[path("work")])
    failed = client.wait_notification(
        lambda note: note.valueType == NotificationPayload.CardViewsUpdated and note.value.failure
    ).value
    assert not failed.views
    assert failed.failure.kind != FailureKind.unspecified
    assert "views are broken" in text(failed.failure.diagnostic)

    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS, sources={
        "default.nix": ARGUMENTS + "{ run = []; views = [ ]; }",
    }).save(client)
    empty = client.wait_notification(
        lambda note: note.valueType == NotificationPayload.CardViewsUpdated and not note.value.failure
    ).value
    assert not empty.views


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
        "default.nix": ARGUMENTS + "{ " + body + " }",
    }).save(client)
    status = wait_run(client, run(client))
    assert status.state == RunState.failed
    assert status.failure.kind == FailureKind.nix_failed
    assert message in text(status.failure.diagnostic)


def test_root_and_external_cards_resolve_their_own_projects(core_factory, project):
    pins = {entry["name"]: entry for entry in fixture_locks()}
    root_lock = [pins["nixpkgs"], dict(pins["nixpkgs"], name="shared"),
                 dict(pins["nixpkgs"], name="root-only")]
    external_lock = [pins["nixpkgs"], dict(pins["tsugumi-kit"], name="shared"),
                     dict(pins["tsugumi-kit"], name="external-only")]
    outputs = 'table First { value:string; } table Outputs { out:First; } root_type Outputs;'
    lookup = 'builtins.hashFile "sha256" <shared/flake.nix>'
    producer = (ARGUMENTS + "{ exports.marker = import ./nested/lookup.nix; run = "
                + synthetic_run('"producer"') + "; }")
    consumer = expression(
        'if (builtins.tryEval <root-only>).success then throw "root name leaked" '
        'else if inputs.data.exports.marker == import ./nested/lookup.nix '
        'then throw "projects shared a mapping" '
        'else if builtins.pathExists <external-only/flake.nix> then "independent" '
        'else throw "external name missing"')
    defs = {
        "producer": {**definition(), "_nix": producer, "_outputs": outputs},
        "work": {**definition(inputs=[{"name": "data"}]), "_nix": consumer, "_outputs": EMPTY_OUTPUTS},
    }
    files = {
        f"definitions/{name}/{file}": content
        for name, ports in (("producer", [("out", first_table("token"))]), ("work", []))
        for file, content in {**synthetic_sources(ports), "nested/lookup.nix": lookup}.items()
    }
    outer = card_in_card("outer", [card("work", "work")], [],
                        {"inputs": {"data": {"card": "work", "port": "data"}}, "outputs": {}})
    write(project, defs, [card("producer", "producer"), outer],
          [conn("producer", "out", "outer", "data")], files, locked=root_lock)
    external = project / "externals/outer"
    from harness.nix_path import declarations
    edit_json(external / "tsugumi.json", lambda doc: doc.update(nixPath=declarations(external_lock)))
    write_lock(external, external_lock)
    client = core_factory().connect()
    assert completed_definition(client, card="outer/work", start="producer")[0] == "independent\n"


def test_target_system_change_selects_new_packages_and_definition(core_factory, project):
    """Changing an available execution system changes the package set and recorded identity."""
    client = core_factory().connect()
    service, = client.call("ProjectInspectRequest").executionServices
    native = text(service.nativeSystem)
    foreign = next((text(s) for s in service.systems if text(s) != native), None)
    if foreign is None:
        pytest.skip("requires a second local system with binfmt_misc and Nix extra-platforms (shared VM)")
    Project(project).card("work", definition(), outputs=EMPTY_OUTPUTS,
                          sources=sources(expression("builtins.currentSystem"))).save(client)
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
