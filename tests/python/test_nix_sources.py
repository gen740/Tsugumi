"""A project opts in to flakes (tsugumi.json nixSources). tsugumid evaluates them
apart from every Card, and a Card gets their packages as inputs.sources.<name>:
derivations it did not evaluate, built when it uses them. A Card still fetches
nothing itself, and has no getFlake. An Agent's save that adds a source waits for
the user. Reading the flake needs the network; nothing of it is built here."""

from __future__ import annotations

import json

import pytest

from conftest import Daemon
from projects import builtin_project, card, card_in_card, conn, run_and_wait, write
from test_permissions import permissions, save
from tsugumi_client import RpcFailure, change, path, text

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


def run(root, *card_path):
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        run_id, _ = run_and_wait(c, ["start"])
        log = lambda stream: text(c.call("LogRangeRequest", runId=run_id, card=path(*card_path), stream=stream, limitBytes=65536).text)
        return log(1), log(2)
    finally:
        d.cleanup()


def test_a_card_gets_the_packages_of_the_flake_its_project_opted_in_to(tmp_path):
    root = tmp_path / "p"
    project(root, "reads", [SOURCE])
    assert f"ciao-4.18.0 {REV}" in run(root, "c")[0]


def test_a_card_that_does_not_name_a_source_never_reads_it(tmp_path):
    root = tmp_path / "p"
    project(root, "plain", [dict(SOURCE, ref=REF.replace(REV, "0" * 40))])
    assert "plain" in run(root, "c")[0]


def test_without_the_opt_in_there_is_no_such_source(tmp_path):
    root = tmp_path / "p"
    project(root, "reads")
    assert "attribute 'ciao' missing" in run(root, "c")[1]


@pytest.mark.parametrize("definition, refused", [
    ("get_flake", "getFlake"),
    ("fetch_tree", "is forbidden in restricted mode"),
])
def test_a_card_itself_cannot_read_a_flake_even_opted_in(tmp_path, definition, refused):
    root = tmp_path / "p"
    project(root, definition, [SOURCE])
    assert refused in run(root, "c")[1]


def test_an_opt_in_stops_at_its_project(tmp_path):
    """A Card in Card is a project of its own: what the outer one opted in to is not its."""
    root = tmp_path / "p"
    write(root, DEFS, [card("start", "start"),
                       card_in_card("inner", [card("c", "reads")], [], {"inputs": {"start": {"card": "c", "port": "start"}}, "outputs": {}})],
          [conn("start", "out", "inner", "start")])
    opt_in(root, [SOURCE])
    assert "attribute 'ciao' missing" in run(root, "inner", "c")[1]


@pytest.mark.parametrize("entry, why", [
    ({"name": "ciao", "ref": REF}, "need name, type and ref"),
    (dict(SOURCE, name="a b"), "is not a name"),
    (dict(SOURCE, type="tarball"), "type must be flake"),
    (dict(SOURCE, ref="github:gen740/ciao"), "must be a locked flake reference"),
    (dict(SOURCE, ref="path:/etc?narHash=sha256-abc"), "must be a locked flake reference"),
])
def test_an_entry_has_to_be_a_locked_flake(tmp_path, entry, why):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        doc = json.loads((root / "tsugumi.json").read_text())
        with pytest.raises(RpcFailure, match=why):
            save(c, [change("SetProject", json=json.dumps(dict(doc, nixSources=[entry])))], origin="user")
    finally:
        d.cleanup()


def test_an_agent_save_adding_a_source_needs_the_users_approval(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        doc = json.loads((root / "tsugumi.json").read_text())

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
    finally:
        d.cleanup()


def test_the_users_own_save_needs_no_approval(tmp_path):
    root = tmp_path / "p"
    builtin_project(root)
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        doc = json.loads((root / "tsugumi.json").read_text())
        save(c, [change("SetProject", json=json.dumps(dict(doc, nixSources=[SOURCE])))], origin="user")
        assert json.loads((root / "tsugumi.json").read_text())["nixSources"] == [SOURCE]
    finally:
        d.cleanup()


def test_an_address_is_looked_up_for_the_settings(tmp_path):
    """A page's URL or a flake reference gives what to save: locked, with the packages. Nothing is saved."""
    root = tmp_path / "p"
    builtin_project(root)
    before = (root / "tsugumi.json").read_text()
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        for address in ("https://github.com/gen740/ciao", "github:gen740/ciao", REF):
            found = c.call("NixSourceResolveRequest", ref=address)
            assert text(found.ref).startswith("github:gen740/ciao/") and "narHash=sha256-" in text(found.ref)
            assert len(text(found.rev)) == 40 and text(found.narHash).startswith("sha256-")
            assert "ciao" in [text(p) for p in found.packages]
        for wrong in ("nixpkgs", "path:/etc", "file:///etc"):
            with pytest.raises(RpcFailure, match="not a flake reference"):
                c.call("NixSourceResolveRequest", ref=wrong)
        assert (root / "tsugumi.json").read_text() == before
    finally:
        d.cleanup()
