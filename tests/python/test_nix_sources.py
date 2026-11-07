"""A project opts in to Nix sources outside nixpkgs (tsugumi.json nixSources): its
Cards read them as inputs.sources.<name>, fetched by hash during evaluation.
Nothing else can be fetched there, a project's opt-in stops at that project, and
an Agent's save that adds one waits for the user. Fetching needs the network."""

from __future__ import annotations

import json

import pytest

from conftest import Daemon
from projects import builtin_project, card, card_in_card, conn, run_and_wait, write
from test_permissions import permissions, save
from tsugumi_client import RpcFailure, change, path, text

URL = "https://github.com/gen740/ciao/archive/0bf0f8bf67a1ffc03ad0234a7aef1eab8681cddb.tar.gz"
HASH = "sha256-1aHjK2Um71mEkR+34H0qMm6LNqAbLJaOJXAUDZ5asd0="
SOURCE = {"name": "ciao", "url": URL, "hash": HASH}
OUTPUTS = "namespace t;\ntable Outputs {}\nroot_type Outputs;\n"
READS_SOURCE = """{ pkgs, inputs, params }:
{ run = ''echo read ${if pkgs.lib.hasInfix "CIAO" (builtins.readFile "${inputs.sources.ciao}/flake.nix") then "ciao" else "other"}''; }
"""
FETCHES = '{ pkgs, inputs, params }:\n{ run = "echo ${builtins.fetchTarball { url = "' + URL + '"; sha256 = "' + HASH + '"; }}"; }\n'
DEFS = {
    "start": {"template": "trigger"},
    "reads": {"language": "bash", "inputs": [{"name": "start"}], "_outputs": OUTPUTS, "_nix": READS_SOURCE},
    "fetches": {"language": "bash", "inputs": [{"name": "start"}], "_outputs": OUTPUTS, "_nix": FETCHES},
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


def test_a_card_reads_the_source_its_project_opted_in_to(tmp_path):
    root = tmp_path / "p"
    project(root, "reads", [SOURCE])
    assert "read ciao" in run(root, "c")[0]


def test_without_the_opt_in_the_same_url_is_refused(tmp_path):
    """Also when an earlier test left it in the store."""
    root = tmp_path / "p"
    project(root, "fetches")
    assert "is forbidden in restricted mode" in run(root, "c")[1]


def test_a_source_has_to_be_the_url_opted_in_to(tmp_path):
    root = tmp_path / "p"
    project(root, "fetches", [dict(SOURCE, url="https://github.com/gen740/ciao/archive/main.tar.gz")])
    assert "is forbidden in restricted mode" in run(root, "c")[1]


def test_an_opt_in_stops_at_its_project(tmp_path):
    """A Card in Card is a project of its own: what the outer one opted in to is not its."""
    root = tmp_path / "p"
    inner = [card("c", "fetches")]
    write(root, DEFS, [card("start", "start"), card_in_card("inner", inner, [], {"inputs": {"start": {"card": "c", "port": "start"}}, "outputs": {}})],
          [conn("start", "out", "inner", "start")])
    opt_in(root, [SOURCE])
    assert "is forbidden in restricted mode" in run(root, "inner", "c")[1]


@pytest.mark.parametrize("entry, why", [
    ({"name": "ciao", "url": URL}, "need name, url and hash"),
    (dict(SOURCE, name="a b"), "is not a name"),
    (dict(SOURCE, url="http://example.com/a.tar.gz"), "must be https"),
    (dict(SOURCE, url=URL + "?x=1"), "must be https"),
    (dict(SOURCE, hash="abc"), "must be sha256"),
])
def test_an_entry_has_to_be_fixed_by_its_hash_over_https(tmp_path, entry, why):
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

        def asked(sources, approved=None):
            with pytest.raises(RpcFailure) as e:
                save(c, [change("SetProject", json=json.dumps(dict(doc, nixSources=sources)))], approved=approved)
            assert e.value.kind == "approval_required"
            return permissions(e.value)

        wanted = [f"nix_source ciao {URL}#{HASH}"]
        assert asked([SOURCE]) == wanted
        save(c, [change("SetProject", json=json.dumps(dict(doc, nixSources=[SOURCE])))], approved=wanted)
        # Another hash is another source, whatever its name; taking one away is not a widening.
        other = dict(SOURCE, hash="sha256-" + "A" * 43 + "=")
        assert asked([other]) == [f"nix_source ciao {URL}#{other['hash']}"]
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
