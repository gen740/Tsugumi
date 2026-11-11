"""A project opts in to flakes (tsugumi.json nixSources). tsugumid evaluates them
apart from every Card, and a Card gets their packages as inputs.sources.<name>:
derivations it did not evaluate, built when it uses them. A Card still fetches
nothing itself, and has no getFlake. An Agent's save that adds a source waits for
the user. Reading the flake needs the network; nothing of it is built here."""

from __future__ import annotations

import json

import pytest

from projects import builtin_project, card, card_in_card, conn, run_and_wait, write
from harness.permissions import permissions, save
from tsugumi_client import RpcFailure, change, path, text
from Tsugumi.Data.RunState import RunState

REV = "0bf0f8bf67a1ffc03ad0234a7aef1eab8681cddb"
REF = f"github:gen740/ciao/{REV}?narHash=sha256-1aHjK2Um71mEkR%2B34H0qMm6LNqAbLJaOJXAUDZ5asd0%3D"
SOURCE = {"name": "ciao", "type": "flake", "ref": REF}
OUTPUTS = "namespace t;\ntable Outputs {}\nroot_type Outputs;\n"


def nix(body: str) -> str:
    return "{ pkgs, inputs, params }:\n" + body + "\n"


DEFS = {
    "start": {"template": "trigger"},
    # The name alone: reading the package does not build it.
    "reads": {"language": "bash", "inputs": [{"name": "start"}], "_outputs": OUTPUTS,
              "_nix": nix('{ run = "echo ${inputs.sources.ciao.packages.ciao.name} ${inputs.sources.ciao.rev}"; }')},
    "plain": {"language": "bash", "inputs": [{"name": "start"}], "_outputs": OUTPUTS, "_nix": nix('{ run = "echo plain"; }')},
    "get_flake": {"language": "bash", "inputs": [{"name": "start"}], "_outputs": OUTPUTS,
                  "_nix": nix(f'{{ run = "echo ${{(builtins.getFlake "{REF}").rev}}"; }}')},
    "fetch_tree": {"language": "bash", "inputs": [{"name": "start"}], "_outputs": OUTPUTS,
                   "_nix": nix(f'{{ run = "echo ${{(builtins.fetchTree "{REF}").rev}}"; }}')},
}


def project(root, definition, sources=None):
    write(root, DEFS, [card("start", "start"), card("c", definition)], [conn("start", "out", "c", "start")])
    if sources:
        opt_in(root, sources)


def opt_in(root, sources):
    doc = json.loads((root / "tsugumi.json").read_text())
    doc["nixSources"] = sources
    (root / "tsugumi.json").write_text(json.dumps(doc))


def run(core_factory, root, *card_path, expected=RunState.succeeded):
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    run_id, status = run_and_wait(c, ["start"], timeout=600)
    log = lambda stream: text(c.call("LogRangeRequest", runId=run_id, card=path(*card_path), stream=stream, limitBytes=65536).text)
    out, err = log(1), log(2)
    assert status.state == expected, (status.failure and text(status.failure.diagnostic), out, err)
    return out, err


def test_a_card_gets_the_packages_of_the_flake_its_project_opted_in_to(core_factory, tmp_path):
    """A card gets the packages of the flake its project opted in to."""
    root = tmp_path / "p"
    project(root, "reads", [SOURCE])
    assert f"ciao-4.18.0 {REV}" in run(core_factory, root, "c")[0]


def test_a_card_that_does_not_name_a_source_never_reads_it(core_factory, tmp_path):
    """A card that does not name a source never reads it."""
    root = tmp_path / "p"
    project(root, "plain", [dict(SOURCE, ref=REF.replace(REV, "0" * 40))])
    assert "plain" in run(core_factory, root, "c")[0]


def test_without_the_opt_in_there_is_no_such_source(core_factory, tmp_path):
    """A card cannot access a source its project has not opted into."""
    root = tmp_path / "p"
    project(root, "reads")
    assert "attribute 'ciao' missing" in run(core_factory, root, "c", expected=RunState.failed)[1]


@pytest.mark.parametrize("definition, refused", [
    ("get_flake", "getFlake"),
    ("fetch_tree", "is forbidden in restricted mode"),
])
def test_a_card_itself_cannot_read_a_flake_even_opted_in(core_factory, tmp_path, definition, refused):
    """Card evaluation cannot invoke getFlake or fetchTree even for an approved source."""
    root = tmp_path / "p"
    project(root, definition, [SOURCE])
    assert refused in run(core_factory, root, "c", expected=RunState.failed)[1]


def test_an_opt_in_stops_at_its_project(core_factory, tmp_path):
    """A Card in Card is a project of its own: what the outer one opted in to is not its."""
    root = tmp_path / "p"
    write(root, DEFS, [card("start", "start"),
                       card_in_card("inner", [card("c", "reads")], [], {"inputs": {"start": {"card": "c", "port": "start"}}, "outputs": {}})],
          [conn("start", "out", "inner", "start")])
    opt_in(root, [SOURCE])
    assert "attribute 'ciao' missing" in run(core_factory, root, "inner", "c", expected=RunState.failed)[1]


@pytest.mark.parametrize("entry, why", [
    ({"name": "ciao", "ref": REF}, "need name, type and ref"),
    (dict(SOURCE, name="a b"), "is not a name"),
    (dict(SOURCE, type="tarball"), "type must be flake"),
    (dict(SOURCE, ref="github:gen740/ciao"), "must be a locked flake reference"),
    (dict(SOURCE, ref="path:/etc?narHash=sha256-abc"), "must be a locked flake reference"),
])
def test_an_entry_has_to_be_a_locked_flake(core_factory, tmp_path, entry, why):
    """An entry has to be a locked flake."""
    root = tmp_path / "p"
    builtin_project(root)
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    doc = json.loads(text(c.call("ProjectInspectRequest").projectJson))
    with pytest.raises(RpcFailure, match=why):
        save(c, [change("SetProject", json=json.dumps(dict(doc, nixSources=[entry])))], origin="user")


def test_an_agent_save_adding_a_source_needs_the_users_approval(core_factory, tmp_path):
    """An agent needs approval to add or change a Nix source."""
    root = tmp_path / "p"
    builtin_project(root)
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    doc = json.loads(text(c.call("ProjectInspectRequest").projectJson))

    def asked(sources):
        with pytest.raises(RpcFailure) as e:
            save(c, [change("SetProject", json=json.dumps(dict(doc, nixSources=sources)))])
        assert e.value.kind == "approval_required"
        return permissions(e.value)

    wanted = [f"nix_source ciao {REF}"]
    assert asked([SOURCE]) == wanted
    save(c, [change("SetProject", json=json.dumps(dict(doc, nixSources=[SOURCE])))], approved=wanted)
    # Another commit is another source; taking one away is not a widening.
    other = dict(SOURCE, ref=REF.replace(REV, "1" * 40))
    assert asked([other]) == [f"nix_source ciao {other['ref']}"]
    save(c, [change("SetProject", json=json.dumps(doc))])


def test_the_users_own_save_needs_no_approval(core_factory, tmp_path):
    """A human save can add a Nix source without an approval prompt."""
    root = tmp_path / "p"
    builtin_project(root)
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    doc = json.loads(text(c.call("ProjectInspectRequest").projectJson))
    save(c, [change("SetProject", json=json.dumps(dict(doc, nixSources=[SOURCE])))], origin="user")
    assert json.loads(text(c.call("ProjectInspectRequest").projectJson))["nixSources"] == [SOURCE]


def test_an_address_is_looked_up_for_the_settings(core_factory, tmp_path):
    """A page's URL or a flake reference gives what to save: locked, with the packages. Nothing is saved."""
    root = tmp_path / "p"
    builtin_project(root)
    daemon = core_factory(project_root=root)
    c = daemon.connect()
    before = c.call("ProjectInspectRequest")
    for address in ("https://github.com/gen740/ciao", "github:gen740/ciao", REF):
        found = c.call("NixSourceResolveRequest", ref=address)
        assert text(found.ref).startswith("github:gen740/ciao/") and "narHash=sha256-" in text(found.ref)
        assert len(text(found.rev)) == 40 and text(found.narHash).startswith("sha256-")
        assert "ciao" in [text(p) for p in found.packages]
    for wrong in ("nixpkgs", "path:/etc", "file:///etc"):
        with pytest.raises(RpcFailure, match="not a flake reference"):
            c.call("NixSourceResolveRequest", ref=wrong)
    after = c.call("ProjectInspectRequest")
    assert after.rev == before.rev and text(after.projectJson) == text(before.projectJson)


def test_external_package_customization_preserves_original_and_changes_derivation(core_factory, tmp_path):
    """Approved JSON overrides run in the flake evaluator and preserve unmodified packages."""
    root = tmp_path / "customized"
    definitions = dict(DEFS)
    definitions["inspect"] = {
        "language": "bash", "inputs": [{"name": "start"}], "_outputs": OUTPUTS,
        "_nix": nix('''let
          original = inputs.sources.original.packages.ciao;
          customized = inputs.sources.customized.packages.ciao;
        in { run = "echo ${original.name} ${customized.name} ${builtins.toString (original.drvPath != customized.drvPath)}"; }'''),
    }
    write(root, definitions, [card("start", "start"), card("c", "inspect")], [conn("start", "out", "c", "start")])
    opt_in(root, [dict(SOURCE, name="original"),
                  dict(SOURCE, name="customized", overrides={"ciao": {"overrideAttrs": {"name": "custom-ciao"}}})])
    assert "ciao-4.18.0 custom-ciao 1" in run(core_factory, root, "c")[0]


def test_customization_reports_unknown_packages(core_factory, tmp_path):
    """An unknown overridden package fails with a usable explanation."""
    root = tmp_path / "unknown-package"
    project(root, "reads", [dict(SOURCE, overrides={"missing": {"overrideAttrs": {"name": "custom"}}})])
    assert "unknown package missing" in run(core_factory, root, "c", expected=RunState.failed)[1]


def test_changing_overrides_requires_user_approval(core_factory, tmp_path):
    """An Agent cannot change an approved source's customization without new approval."""
    root = tmp_path / "approval"
    project(root, "reads", [SOURCE])
    c = core_factory(project_root=root).connect()
    doc = json.loads(text(c.call("ProjectInspectRequest").projectJson))
    doc["nixSources"][0]["overrides"] = {"ciao": {"overrideAttrs": {"version": "1", "name": "changed"}}}
    with pytest.raises(RpcFailure) as error:
        save(c, [change("SetProject", json=json.dumps(doc))], origin="agent")
    assert error.value.kind == "approval_required"
    requested = permissions(error.value)
    assert any("overrides" in permission for permission in requested)
    # Approval of the original flake does not approve a different source URL,
    # patches, or build arguments for its customized package.
    with pytest.raises(RpcFailure) as old_approval:
        save(c, [change("SetProject", json=json.dumps(doc))], approved=[f"nix_source ciao {REF}"])
    assert old_approval.value.kind == "approval_required"
    save(c, [change("SetProject", json=json.dumps(doc))], approved=requested)
    assert json.loads(text(c.call("ProjectInspectRequest").projectJson))["nixSources"] == doc["nixSources"]
    save(c, [change("SetProject", json=json.dumps(doc))])
    save(c, [change("SetProject", json=json.dumps(doc, sort_keys=True, indent=2))])


def test_overridable_fixture_builds_with_pinned_source_and_patch(core_factory, tmp_path, monkeypatch):
    """A Card runs the customized package with pinned downloads and keeps its original usable."""
    from harness.nix_source import source_fixture
    with source_fixture(tmp_path, monkeypatch) as (source, fetched):
        root = tmp_path / "fixture-project"
        definitions = dict(DEFS)
        definitions["inspect"] = {
            "language": "bash", "inputs": [{"name": "start"}], "_outputs": OUTPUTS,
            "_nix": nix('''{
              run = "cat ${inputs.sources.fixture.packages.sample}/value ${inputs.sources.original.packages.sample}/value";
            }'''),
        }
        write(root, definitions, [card("start", "start"), card("c", "inspect")], [conn("start", "out", "c", "start")])
        custom = {"sample": {"override": {"message": "custom", "src": fetched("source.txt")},
                             "overrideAttrs": {"patches": [fetched("patch.txt")]}}}
        opt_in(root, [dict(source, overrides=custom), dict(source, name="original")])
        c = core_factory(project_root=root).connect()
        def output():
            run_id, status = run_and_wait(c, ["start"], timeout=120)
            assert status.state == RunState.succeeded, status.failure and text(status.failure.diagnostic)
            return text(c.call("LogRangeRequest", runId=run_id, card=path("c"), stream=1).text)
        assert output() == "custom\npinned source\npinned patch\noriginal\n"
        # The same process has cached both packages. A new override must not
        # reuse the earlier customized result or alter the unmodified source.
        custom["sample"]["override"]["message"] = "changed"
        doc = json.loads(text(c.call("ProjectInspectRequest").projectJson))
        doc["nixSources"][0]["overrides"] = custom
        save(c, [change("SetProject", json=json.dumps(doc))], origin="user")
        assert output() == "changed\npinned source\npinned patch\noriginal\n"
        custom["sample"]["override"]["src"]["fetchurl"]["sha256"] = "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
        doc["nixSources"][0]["overrides"] = custom
        save(c, [change("SetProject", json=json.dumps(doc))], origin="user")
        run_id, status = run_and_wait(c, ["start"], timeout=120)
        assert status.state == RunState.failed
        assert "hash mismatch" in text(status.failure.diagnostic)


@pytest.mark.parametrize("customization", [
    {"ciao": {"override": {}}},
    {"ciao": {"overrideAttrs": {"src": {"fetchurl": {"url": "https://invalid.example/source"}}}}},
])
def test_unsupported_customization_has_an_actionable_error(core_factory, tmp_path, customization):
    """Unsupported functions and unpinned fetches explain which configuration to change."""
    root = tmp_path / "unsupported"
    project(root, "reads", [dict(SOURCE, overrides=customization)])
    _, error = run(core_factory, root, "c", expected=RunState.failed)
    expected = "does not support override" if "override" in customization["ciao"] else "fetchurl needs url and sha256"
    assert expected in error


@pytest.mark.parametrize("overrides", [[], "let pkgs = import <nixpkgs>; in pkgs", {"ciao": {"override": "x: x"}},
                                       {"ciao": {"overrideAttrs": {}, "unknown": {}}}])
def test_invalid_override_shapes_are_rejected_before_saving(core_factory, tmp_path, overrides):
    """The customization boundary accepts JSON argument maps, never arbitrary Nix expressions."""
    root = tmp_path / "invalid-override"
    builtin_project(root)
    c = core_factory(project_root=root).connect()
    before = c.call("ProjectInspectRequest")
    doc = json.loads(text(before.projectJson))
    doc["nixSources"] = [dict(SOURCE, overrides=overrides)]
    with pytest.raises(RpcFailure) as error:
        save(c, [change("SetProject", json=json.dumps(doc))], origin="user")
    assert error.value.kind == "invalid_document"
    assert c.call("ProjectInspectRequest").rev == before.rev


def test_nested_project_cannot_inherit_outer_customizations(core_factory, tmp_path):
    """External package customizations remain scoped to the project that approved them."""
    root = tmp_path / "scoped"
    write(root, DEFS, [card("start", "start"),
                       card_in_card("inner", [card("c", "reads")], [],
                                    {"inputs": {"start": {"card": "c", "port": "start"}}, "outputs": {}})],
          [conn("start", "out", "inner", "start")])
    opt_in(root, [dict(SOURCE, overrides={"ciao": {"overrideAttrs": {"name": "outer-only"}}})])
    assert "attribute 'ciao' missing" in run(core_factory, root, "inner", "c", expected=RunState.failed)[1]


def test_card_override_attempt_points_to_supported_project_configuration(core_factory, tmp_path):
    """A direct Card-side override explains the supported approved customization API."""
    root = tmp_path / "card-side-override"
    definitions = dict(DEFS)
    definitions["custom"] = {"language": "bash", "inputs": [{"name": "start"}], "_outputs": OUTPUTS,
                             "_nix": nix('''let p = inputs.sources.ciao.packages.ciao.overrideAttrs (_: { name = "custom"; });
                                            in { run = "echo ${p.name}"; }''')}
    write(root, definitions, [card("start", "start"), card("c", "custom")], [conn("start", "out", "c", "start")])
    opt_in(root, [SOURCE])
    assert "nixSources[].overrides.<package>.overrideAttrs" in run(core_factory, root, "c", expected=RunState.failed)[1]
