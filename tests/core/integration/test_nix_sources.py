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
