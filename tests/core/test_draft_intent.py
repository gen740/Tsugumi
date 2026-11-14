"""A save from a model (origin "agent") that implements a Draft leaves its intent
complete in _meta.json: a description, a context when the Draft had something
to write it from, and everything the Draft had, unchanged. Otherwise nothing is
saved and the failure says what to write, also inside a Card in Card. The user's
own saves are theirs, and so is a Draft that stays a Draft."""

from __future__ import annotations

import json

import pytest

from projects import builtin_project, card, card_in_card, write
from harness.permissions import permissions, save
from tsugumi_client import RpcFailure, change, text

PLAN = {"description": "Count words", "content": "Split the text and count.", "outputs": [{"name": "summary"}]}
IMPLEMENTED = {"template": "fork"}


def implement(meta=None, cid="plan", body=IMPLEMENTED, **fills):
    if meta is None:
        return change("PutDefinition", id=cid, cardJson=json.dumps(body), keepMeta=True, **fills)
    return change("PutDefinition", id=cid, cardJson=json.dumps(body), metaJson=json.dumps(meta))


def meta_of(c, cid="plan"):
    d = next(d for d in c.call("ProjectInspectRequest").definitions if text(d.id) == cid)
    return json.loads(text(d.metaJson)) if text(d.metaJson) else None


def is_draft(c, cid="plan"):
    d = next(d for d in c.call("ProjectInspectRequest").definitions if text(d.id) == cid)
    return json.loads(text(d.cardJson)).get("template") == "draft"


def refused(c, changes, *expected):
    """The save fails with validation_failed naming each expected word, and stores nothing."""
    rev = c.call("ProjectInspectRequest").rev
    with pytest.raises(RpcFailure) as e:
        save(c, changes)
    assert e.value.kind == "validation_failed", e.value
    problems = [text(kv.value) for kv in e.value.failure.args if text(kv.key) == "problem"]
    assert len(problems) == len(expected), problems
    for word, problem in zip(expected, problems):
        assert word in problem, problems
        assert problem in text(e.value.failure.diagnostic)
    assert c.call("ProjectInspectRequest").rev == rev
    return problems


@pytest.fixture
def draft(tmp_path, core_factory):
    def start(meta=PLAN):
        root = tmp_path / "p"
        builtin_project(root, extra_cards=[card("plan")])
        if meta is not None:
            (root / "definitions/plan/_meta.json").write_text(json.dumps(meta))
        return core_factory(project_root=root).connect()

    return start


def test_keeping_the_meta_of_a_draft_with_no_context_is_refused(draft):
    """Implementing a draft without its required context preserves the draft and reports validation failure."""
    c = draft()
    assert "plan: context is empty" in refused(c, [implement()], "context")[0]
    assert is_draft(c)
    assert meta_of(c) == PLAN


def test_a_filled_context_goes_through_and_keeps_all_the_draft_had(draft):
    """Adding the required context allows implementation while preserving the draft's original intent."""
    c = draft()
    full = dict(PLAN, context="The user wants the count for the report.")
    save(c, [implement(full)])
    assert not is_draft(c)
    assert meta_of(c) == full


def test_keep_meta_is_enough_when_nothing_is_empty(draft):
    """A complete draft intent can be preserved unchanged when the card is implemented."""
    full = dict(PLAN, context="Already written.")
    c = draft(full)
    save(c, [implement()])
    assert not is_draft(c)
    assert meta_of(c) == full


def test_an_empty_description_is_refused_and_a_blank_one_counts_as_empty(draft):
    """An empty description is refused and a blank one counts as empty."""
    c = draft({"content": "Split the text and count.", "context": "why"})
    refused(c, [implement()], "description")
    refused(c, [implement({"description": "  \n", "content": "Split the text and count.", "context": "why"})], "description")
    save(c, [implement({"description": "Counts words", "content": "Split the text and count.", "context": "why"})])
    assert meta_of(c)["description"] == "Counts words"


def test_a_draft_is_implemented_by_filling_only_what_is_empty(draft):
    """The model need not write the Draft's words again, so it cannot change them."""
    c = draft()
    save(c, [implement(None, fillContext="The user wants the count for the report.", fillDescription="Ignored: it has text")])
    assert not is_draft(c)
    assert meta_of(c) == dict(PLAN, context="The user wants the count for the report.")


def test_filling_writes_a_description_only_where_there_is_none(draft):
    """Filling writes a description only where there is none."""
    c = draft({"content": "Split the text and count."})
    save(c, [implement(None, fillDescription="Counts words", fillContext="For the report.")])
    assert meta_of(c) == {"content": "Split the text and count.", "description": "Counts words", "context": "For the report."}


def test_what_the_draft_had_may_not_change(draft):
    """Implementing a draft cannot rewrite or delete its existing description, content or context."""
    full = dict(PLAN, context="Written by the user.")
    c = draft(full)
    refused(c, [implement(dict(full, description="Another summary"))], "description changed")
    refused(c, [implement(dict(full, content="Another plan"))], "content changed")
    refused(c, [implement(dict(full, context="Another reason"))], "context changed")
    # Dropping a field is changing it (the empty description is reported too).
    gone = {k: v for k, v in full.items() if k != "description"}
    refused(c, [implement(gone)], "description changed", "description is empty")
    assert is_draft(c)


def test_with_nothing_to_go_on_the_context_may_stay_empty_but_the_description_may_not(draft):
    """With nothing to go on the context may stay empty but the description may not."""
    c = draft(None)
    refused(c, [implement()], "description")
    save(c, [implement({"description": "Logs what it gets"})])
    assert meta_of(c) == {"description": "Logs what it gets"}


def test_a_renamed_draft_is_still_a_draft_to_implement(draft):
    """Renaming a draft cannot bypass its implementation requirements."""
    c = draft()
    from test_definitions import rename
    rename(c, "plan", "count")
    assert "plan: context is empty" in refused(c, [implement(cid="plan")], "context")[0]
    refused(c, [implement({"name": "other"}, cid="plan")], "context")


def test_only_a_model_implementing_a_draft_is_asked(draft):
    """Intent validation applies to agent implementations while allowing drafts and human edits."""
    c = draft()
    # A Draft that stays one: the model may leave it unfinished.
    save(c, [change("PutDefinition", id="plan", cardJson=json.dumps({"template": "draft", "inputs": [{"name": "in"}]}), keepMeta=True)])
    assert is_draft(c)
    # Any other Card: nothing to keep.
    body = json.loads(next(text(d.cardJson) for d in c.call("ProjectInspectRequest").definitions if text(d.id) == "out"))
    save(c, [change("PutDefinition", id="out", cardJson=json.dumps(body), keepMeta=True)])
    # The user's own save is theirs.
    save(c, [implement()], origin="ui")
    assert not is_draft(c)
    assert meta_of(c) == PLAN


def test_the_user_is_not_asked_to_approve_a_save_that_would_be_refused(draft):
    """Invalid draft intent is rejected before requesting permission approval."""
    c = draft()
    wide = dict(IMPLEMENTED, network=True)
    refused(c, [implement(body=wide)], "context")
    with pytest.raises(RpcFailure) as e:
        save(c, [implement(dict(PLAN, context="why"), body=wide)])
    assert e.value.kind == "approval_required"
    assert permissions(e.value) == ["network plan"]


def test_a_draft_in_a_card_in_card_is_checked_too(tmp_path, core_factory):
    """Nested draft implementations preserve intent and require their missing context."""
    root = tmp_path / "p"
    rel = "externals/inner/definitions/plan/"
    write(root, {}, [card_in_card("inner", [card("plan")], [], {"inputs": {}, "outputs": {}})], [],
          {rel + "_meta.json": json.dumps(PLAN)})
    d = core_factory(project_root=root)
    c = d.connect()
    hashes = {text(s.path): text(s.hash) for s in c.call("ProjectInspectRequest").sources}
    card_file = change("PutSource", path=rel + "_card.json", bytes=list(json.dumps(IMPLEMENTED).encode()),
                       baseHash=hashes[rel + "_card.json"])

    def meta_file(meta):
        return change("PutSource", path=rel + "_meta.json", bytes=list(json.dumps(meta).encode()),
                      baseHash=hashes[rel + "_meta.json"])

    assert "inner/plan: context is empty" in refused(c, [card_file], "context")[0]
    refused(c, [card_file, meta_file(dict(PLAN, context="why", description="Another"))], "description changed")
    save(c, [card_file, meta_file(dict(PLAN, context="why"))])
    assert json.loads(bytes(c.call("ProjectFileReadRequest", path=rel + "_card.json").bytes)) == IMPLEMENTED
    assert json.loads(bytes(c.call("ProjectFileReadRequest", path=rel + "_meta.json").bytes))["context"] == "why"
