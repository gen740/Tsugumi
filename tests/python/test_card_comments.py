"""Comments on Cards live in _meta.json, for the user and the Agent alike: a model's
save may add, edit, resolve or delete them. What is kept is who did what: such a
save is stamped from its origin, whatever the model wrote, and tells what it did.
A comment changes nothing about what runs, and each version says what it did to
the comments."""

from __future__ import annotations

import json
import subprocess

from conftest import Daemon
from projects import builtin_project, card, card_in_card, write
from test_permissions import save
from tsugumi_client import _request_class, change, new_id, text

import pytest

ASK = {"id": "c1", "text": "Use a log scale", "author": "user"}
OTHER = {"id": "c2", "text": "Name the axis", "author": "user"}


def put(comments, cid="plan", body=None, **meta):
    return change("PutDefinition", id=cid, cardJson=json.dumps(body or {"template": "logger"}),
                  metaJson=json.dumps({"comments": comments, **meta}))


def comments_of(c, cid="plan"):
    d = next(d for d in c.call("ProjectInspectRequest").definitions if text(d.id) == cid)
    return json.loads(text(d.metaJson)).get("comments") if text(d.metaJson) else None


def told(result):
    return [(text(x.card), text(x.text), text(x.change), text(x.by) if x.by else None) for x in result.comments]


@pytest.fixture
def project(tmp_path):
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("plan", "log")])
    (root / "definitions/plan/_meta.json").write_text(json.dumps({"description": "Plots", "comments": [ASK, OTHER]}))
    d = Daemon(root).start()
    c = d.client()
    c.hello()
    yield c
    d.cleanup()


def test_a_model_resolving_a_comment_is_recorded_with_what_it_changed(project):
    done = {**ASK, "resolved": True, "resolution": "Logarithmic y axis"}
    result = save(project, [put([done, OTHER], body={"template": "logger", "markdown": True}, description="Plots")])
    assert comments_of(project) == [{**done, "stateBy": "agent", "cards": ["plan"]}, OTHER]
    assert told(result) == [("plan", "Use a log scale", "resolved", "agent")]
    assert text(result.comments[0].resolution) == "Logarithmic y axis"
    assert [text(k) for k in result.comments[0].cards] == ["plan"]


def test_resolving_without_changing_a_card_names_no_card(project):
    save(project, [put([{**ASK, "resolved": True, "resolution": "Nothing to do"}, OTHER], description="Plots")])
    first = comments_of(project)[0]
    assert first["stateBy"] == "agent" and "cards" not in first


def test_a_model_may_write_a_comment_and_it_says_who_wrote_it(project):
    result = save(project, [put([ASK, OTHER, {"text": "Check the units"}])])
    third = comments_of(project)[2]
    assert third["text"] == "Check the units" and third["author"] == "agent" and third["id"]
    assert told(result) == [("plan", "Check the units", "added", "agent")]


def test_what_a_model_says_about_who_did_it_does_not_count(project):
    save(project, [put([{**ASK, "author": "agent"}, {**OTHER, "author": "user"}, {"id": "c3", "text": "Mine", "author": "user"},
                        {"id": "c4", "text": "Done", "author": "user", "resolved": True, "stateBy": "user", "cards": ["x"]}])])
    got = comments_of(project)
    assert [c.get("author") for c in got] == ["user", "user", "agent", "agent"]
    assert got[3]["stateBy"] == "agent" and "cards" not in got[3]


def test_a_model_may_edit_a_comment_and_it_shows(project):
    result = save(project, [put([{**ASK, "text": "Use a linear scale"}, OTHER])])
    assert comments_of(project)[0] == {**ASK, "text": "Use a linear scale", "editedBy": "agent"}
    assert told(result) == [("plan", "Use a linear scale", "edited", "agent")]


def test_a_model_may_delete_a_comment_and_it_says_so(project):
    result = save(project, [put([ASK])])
    assert comments_of(project) == [ASK]
    assert told(result) == [("plan", "Name the axis", "removed", None)]


def test_keep_meta_keeps_the_comments(project):
    result = save(project, [change("PutDefinition", id="plan", cardJson=json.dumps({"template": "logger", "markdown": True}), keepMeta=True)])
    assert comments_of(project) == [ASK, OTHER]
    assert not result.comments


def test_two_comments_with_one_id_are_kept_apart(project):
    save(project, [put([ASK, OTHER, {"id": "c1", "text": "Twin"}])])
    ids = [c["id"] for c in comments_of(project)]
    assert len(set(ids)) == 3 and ids[0] == "c1"


def test_comments_follow_a_renamed_card(project):
    snap = project.call("ProjectInspectRequest")
    doc = json.loads(text(snap.projectJson))
    for k in doc["canvas"]["cards"]:
        if k["id"] == "plan":
            k["id"] = "plot"
    result = save(project, [change("RenameDefinition", id="plan", to="plot"), change("SetProject", json=json.dumps(doc))])
    assert comments_of(project, "plot") == [ASK, OTHER]
    assert not result.comments, "the same comments: nothing happened to them"


def test_a_comment_moved_to_another_card_is_the_same_comment(project):
    snap = project.call("ProjectInspectRequest")
    doc = json.loads(text(snap.projectJson))
    doc["canvas"]["cards"].append({"id": "split"})
    result = save(project, [put([OTHER]), put([ASK], cid="split"), change("SetProject", json=json.dumps(doc))])
    assert comments_of(project, "split") == [ASK]
    assert not result.comments


def test_a_card_removed_takes_its_comments_and_says_so(project):
    snap = project.call("ProjectInspectRequest")
    doc = json.loads(text(snap.projectJson))
    doc["canvas"]["cards"] = [k for k in doc["canvas"]["cards"] if k["id"] != "plan"]
    result = save(project, [change("RemoveDefinition", id="plan"), change("SetProject", json=json.dumps(doc))])
    assert {text(x.change) for x in result.comments} == {"removed"}
    assert not any(text(d.id) == "plan" for d in project.call("ProjectInspectRequest").definitions)


def test_a_save_from_a_person_is_taken_as_it_is(project):
    mine = {"id": "c3", "text": "Mine", "author": "user", "stateBy": "user", "resolved": True}
    result = save(project, [put([{**ASK, "text": "Reworded", "editedBy": "user"}, mine])], origin="ui:1")
    assert comments_of(project) == [{**ASK, "text": "Reworded", "editedBy": "user"}, mine]
    assert not result.comments, "only a model is told"
    save(project, [put([])], origin="ui:1")
    assert comments_of(project) == []


def test_a_comment_changes_nothing_about_what_runs(project):
    before = project.call("ProjectInspectRequest")
    save(project, [put([ASK, OTHER, {"id": "c3", "text": "Mine"}], description="Plots")], origin="ui:1")
    after = project.call("ProjectInspectRequest")
    assert after.rev != before.rev
    assert after.executionRev == before.executionRev


def test_a_draft_takes_comments_as_any_card(tmp_path):
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("plan")])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        save(c, [put([{"text": "Think about units"}], body={"template": "draft"}, description="A plan")])
        assert comments_of(c)[0]["author"] == "agent"
        assert json.loads(text(next(x for x in c.call("ProjectInspectRequest").definitions if text(x.id) == "plan").cardJson))["template"] == "draft"
    finally:
        d.cleanup()


def test_comments_inside_a_card_in_card_are_recorded_too(tmp_path):
    root = tmp_path / "p"
    write(root, {"log": {"template": "logger"}}, [card_in_card("outer", [card("plan", "log")], [], {"inputs": {}, "outputs": {}})], [])
    (root / "externals/outer/definitions/plan/_meta.json").write_text(json.dumps({"comments": [ASK]}))
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        path = "externals/outer/definitions/plan/_meta.json"
        base = text(c.call("ProjectFileReadRequest", path=path).hash)
        result = save(c, [change("PutSource", path=path, baseHash=base, bytes=json.dumps(
            {"comments": [{**ASK, "resolved": True, "resolution": "Done", "stateBy": "user"}, {"text": "Also"}]}).encode())])
        assert sorted(told(result)) == [("outer/plan", "Also", "added", "agent"), ("outer/plan", "Use a log scale", "resolved", "agent")]
        now = json.loads(bytes(c.call("ProjectFileReadRequest", path=path).bytes).decode())["comments"]
        assert now[0]["stateBy"] == "agent" and now[1]["author"] == "agent"
    finally:
        d.cleanup()


def changes_of(commit):
    return [(text(x.card), text(x.text), text(x.change), text(x.by) if x.by else None, text(x.resolution) if x.resolution else None,
             [text(k) for k in x.cards]) for x in commit.comments]


def test_each_version_says_what_it_did_to_the_comments(tmp_path):
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("plan", "log")])
    d = Daemon(root).start()
    try:
        c = d.client()
        c.hello()
        c.call("VersionInitRequest", opId=new_id(), origin="user")

        def mark(name):
            # (the request's own field is `name`, as call()'s first argument)
            m = _request_class("VersionMarkRequest")()
            m.opId, m.origin, m.name, m.message = new_id(), "user", name, name
            c.wait_reply(c.send("VersionMarkRequest", m))

        def at(name):
            tagged = [v for v in c.call("VersionGraphRequest").commits if name in [text(t) for t in v.tags]]
            assert tagged, name
            return changes_of(tagged[0])

        save(c, [put([ASK, OTHER])], origin="ui:1")
        mark("written")
        assert sorted(at("written")) == [
            ("plan", "Name the axis", "added", "user", None, []),
            ("plan", "Use a log scale", "added", "user", None, []),
        ]

        save(c, [put([{**ASK, "resolved": True, "resolution": "Log y"}, OTHER], body={"template": "logger", "markdown": True})])
        mark("resolved")
        assert at("resolved") == [("plan", "Use a log scale", "resolved", "agent", "Log y", ["plan"])]

        save(c, [put([{**ASK, "stateBy": "user"}, {**OTHER, "text": "Name both axes", "editedBy": "user"}])], origin="ui:1")
        mark("reopened")
        assert sorted(at("reopened")) == [
            ("plan", "Name both axes", "edited", "user", None, []),
            ("plan", "Use a log scale", "reopened", "user", None, []),
        ]

        save(c, [put([{"text": "Asked and done", "resolved": True, "resolution": "Did it"}])])
        mark("replaced")
        assert sorted(at("replaced")) == [
            ("plan", "Asked and done", "resolved", "agent", "Did it", []),
            ("plan", "Name both axes", "removed", None, None, []),
            ("plan", "Use a log scale", "removed", None, None, []),
        ]

        # git holds the same: the comment is in the version's own _meta.json.
        shown = subprocess.run(["git", "-C", str(root), "show", "written:definitions/plan/_meta.json"],
                               capture_output=True, text=True, check=True).stdout
        assert "Use a log scale" in shown
    finally:
        d.cleanup()
