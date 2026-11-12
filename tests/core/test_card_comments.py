"""Comments on Cards live in _meta.json, for the user and the Agent alike: a model's
save may add, edit, resolve or delete them. What is kept is who did what: such a
save is stamped from its origin, whatever the model wrote, and tells what it did.
A comment changes nothing about what runs, and each version says what it did to
the comments."""

from __future__ import annotations

import json

from projects import SINK, builtin_project, card, card_in_card, write
from harness.permissions import save
from tsugumi_client import _request_class, change, new_id, text

import pytest

ASK = {"id": "c1", "text": "Use a log scale", "author": "user"}
OTHER = {"id": "c2", "text": "Name the axis", "author": "user"}


def put(comments, cid="plan", body=None, **meta):
    return change("PutDefinition", id=cid, cardJson=json.dumps(body or SINK),
                  metaJson=json.dumps({"comments": comments, **meta}))


def comments_of(c, cid="plan"):
    d = next(d for d in c.call("ProjectInspectRequest").definitions if text(d.id) == cid)
    return json.loads(text(d.metaJson)).get("comments") if text(d.metaJson) else None


def told(result):
    return [(text(x.card), text(x.text), text(x.change), text(x.by) if x.by else None) for x in result.comments]


@pytest.fixture
def comment_client(tmp_path, core_factory):
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("plan", "sink")])
    (root / "definitions/plan/_meta.json").write_text(json.dumps({"description": "Plots", "comments": [ASK, OTHER]}))
    d = core_factory(project_root=root)
    c = d.connect()
    return c


def test_a_model_resolving_a_comment_is_recorded_with_what_it_changed(comment_client):
    """A model resolving a comment is recorded with what it changed."""
    done = {**ASK, "resolved": True, "resolution": "Logarithmic y axis"}
    result = save(comment_client, [put([done, OTHER], body=dict(SINK, maxRuns=2), description="Plots")])
    assert comments_of(comment_client) == [{**done, "stateBy": "agent", "cards": ["plan"]}, OTHER]
    assert told(result) == [("plan", "Use a log scale", "resolved", "agent")]
    assert text(result.comments[0].resolution) == "Logarithmic y axis"
    assert [text(k) for k in result.comments[0].cards] == ["plan"]


def test_resolving_without_changing_a_card_names_no_card(comment_client):
    """Resolving without changing a card names no card."""
    save(comment_client, [put([{**ASK, "resolved": True, "resolution": "Nothing to do"}, OTHER], description="Plots")])
    first = comments_of(comment_client)[0]
    assert first["stateBy"] == "agent" and "cards" not in first


def test_a_model_may_write_a_comment_and_it_says_who_wrote_it(comment_client):
    """A model may write a comment and it says who wrote it."""
    result = save(comment_client, [put([ASK, OTHER, {"text": "Check the units"}])])
    third = comments_of(comment_client)[2]
    assert third["text"] == "Check the units" and third["author"] == "agent" and third["id"]
    assert told(result) == [("plan", "Check the units", "added", "agent")]


def test_what_a_model_says_about_who_did_it_does_not_count(comment_client):
    """An agent cannot impersonate a human author or resolver through comment metadata."""
    save(comment_client, [put([{**ASK, "author": "agent"}, {**OTHER, "author": "user"}, {"id": "c3", "text": "Mine", "author": "user"},
                        {"id": "c4", "text": "Done", "author": "user", "resolved": True, "stateBy": "user", "cards": ["x"]}])])
    got = comments_of(comment_client)
    assert [c.get("author") for c in got] == ["user", "user", "agent", "agent"]
    assert got[3]["stateBy"] == "agent" and "cards" not in got[3]


def test_a_model_may_edit_a_comment_and_it_shows(comment_client):
    """Editing a comment records the agent as its editor and reports the change."""
    result = save(comment_client, [put([{**ASK, "text": "Use a linear scale"}, OTHER])])
    assert comments_of(comment_client)[0] == {**ASK, "text": "Use a linear scale", "editedBy": "agent"}
    assert told(result) == [("plan", "Use a linear scale", "edited", "agent")]


def test_a_model_may_delete_a_comment_and_it_says_so(comment_client):
    """A model may delete a comment and it says so."""
    result = save(comment_client, [put([ASK])])
    assert comments_of(comment_client) == [ASK]
    assert told(result) == [("plan", "Name the axis", "removed", None)]


def test_keep_meta_keeps_the_comments(comment_client):
    """Keep meta keeps the comments."""
    result = save(comment_client, [change("PutDefinition", id="plan", cardJson=json.dumps(dict(SINK, maxRuns=2)), keepMeta=True)])
    assert comments_of(comment_client) == [ASK, OTHER]
    assert not result.comments


def test_two_comments_with_one_id_are_kept_apart(comment_client):
    """Two comments with one id are kept apart."""
    save(comment_client, [put([ASK, OTHER, {"id": "c1", "text": "Twin"}])])
    ids = [c["id"] for c in comments_of(comment_client)]
    assert len(set(ids)) == 3 and ids[0] == "c1"


def test_comments_follow_a_renamed_card(comment_client):
    """Comments follow a renamed card."""
    snap = comment_client.call("ProjectInspectRequest")
    doc = json.loads(text(snap.projectJson))
    for k in doc["canvas"]["cards"]:
        if k["id"] == "plan":
            k["id"] = "plot"
    result = save(comment_client, [change("RenameDefinition", id="plan", to="plot"), change("SetProject", json=json.dumps(doc))])
    assert comments_of(comment_client, "plot") == [ASK, OTHER]
    assert not result.comments, "the same comments: nothing happened to them"


def test_a_comment_moved_to_another_card_is_the_same_comment(comment_client):
    """A comment moved to another card is the same comment."""
    snap = comment_client.call("ProjectInspectRequest")
    doc = json.loads(text(snap.projectJson))
    doc["canvas"]["cards"].append({"id": "split"})
    result = save(comment_client, [put([OTHER]), put([ASK], cid="split"), change("SetProject", json=json.dumps(doc))])
    assert comments_of(comment_client, "split") == [ASK]
    assert not result.comments


def test_a_card_removed_takes_its_comments_and_says_so(comment_client):
    """A card removed takes its comments and says so."""
    snap = comment_client.call("ProjectInspectRequest")
    doc = json.loads(text(snap.projectJson))
    doc["canvas"]["cards"] = [k for k in doc["canvas"]["cards"] if k["id"] != "plan"]
    result = save(comment_client, [change("RemoveDefinition", id="plan"), change("SetProject", json=json.dumps(doc))])
    assert {text(x.change) for x in result.comments} == {"removed"}
    assert not any(text(d.id) == "plan" for d in comment_client.call("ProjectInspectRequest").definitions)


def test_a_save_from_a_person_is_taken_as_it_is(comment_client):
    """A human save preserves the supplied comment attribution and can remove all comments."""
    mine = {"id": "c3", "text": "Mine", "author": "user", "stateBy": "user", "resolved": True}
    result = save(comment_client, [put([{**ASK, "text": "Reworded", "editedBy": "user"}, mine])], origin="ui:1")
    assert comments_of(comment_client) == [{**ASK, "text": "Reworded", "editedBy": "user"}, mine]
    assert not result.comments, "only a model is told"
    save(comment_client, [put([])], origin="ui:1")
    assert comments_of(comment_client) == []


def test_a_comment_changes_nothing_about_what_runs(comment_client):
    """A comment changes nothing about what runs."""
    before = comment_client.call("ProjectInspectRequest")
    save(comment_client, [put([ASK, OTHER, {"id": "c3", "text": "Mine"}], description="Plots")], origin="ui:1")
    after = comment_client.call("ProjectInspectRequest")
    assert after.rev != before.rev
    assert after.executionRev == before.executionRev


def test_a_draft_takes_comments_as_any_card(tmp_path, core_factory):
    """A draft takes comments as any card."""
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("plan")])
    d = core_factory(project_root=root)
    c = d.connect()
    save(c, [put([{"text": "Think about units"}], body={"template": "draft"}, description="A plan")])
    assert comments_of(c)[0]["author"] == "agent"
    assert json.loads(text(next(x for x in c.call("ProjectInspectRequest").definitions if text(x.id) == "plan").cardJson))["template"] == "draft"


def test_comments_inside_a_card_in_card_are_recorded_too(tmp_path, core_factory):
    """Comments inside a card in card are recorded too."""
    root = tmp_path / "p"
    write(root, {"sink": SINK}, [card_in_card("outer", [card("plan", "sink")], [], {"inputs": {}, "outputs": {}})], [])
    (root / "externals/outer/definitions/plan/_meta.json").write_text(json.dumps({"comments": [ASK]}))
    d = core_factory(project_root=root)
    c = d.connect()
    path = "externals/outer/definitions/plan/_meta.json"
    base = text(c.call("ProjectFileReadRequest", path=path).hash)
    result = save(c, [change("PutSource", path=path, baseHash=base, bytes=json.dumps(
        {"comments": [{**ASK, "resolved": True, "resolution": "Done", "stateBy": "user"}, {"text": "Also"}]}).encode())])
    assert sorted(told(result)) == [("outer/plan", "Also", "added", "agent"), ("outer/plan", "Use a log scale", "resolved", "agent")]
    now = json.loads(bytes(c.call("ProjectFileReadRequest", path=path).bytes).decode())["comments"]
    assert now[0]["stateBy"] == "agent" and now[1]["author"] == "agent"


def changes_of(commit):
    return [(text(x.card), text(x.text), text(x.change), text(x.by) if x.by else None, text(x.resolution) if x.resolution else None,
             [text(k) for k in x.cards]) for x in commit.comments]


def test_each_version_says_what_it_did_to_the_comments(tmp_path, core_factory):
    """Each version says what it did to the comments."""
    root = tmp_path / "p"
    builtin_project(root, extra_cards=[card("plan", "sink")])
    d = core_factory(project_root=root)
    c = d.connect()
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

    save(c, [put([{**ASK, "resolved": True, "resolution": "Log y"}, OTHER], body=dict(SINK, maxRuns=2))])
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

    written = next(v for v in c.call("VersionGraphRequest").commits if "written" in [text(t) for t in v.tags])
    c.call("VersionCheckoutRequest", baseRev=c.call("ProjectInspectRequest").rev,
           opId=new_id(), origin="ui:test", commit=text(written.id))
    assert comments_of(c) == [ASK, OTHER]
