# Editing settings and revision exclusion

A working copy belongs to an editor. Its revision is the revision the person
reads, or the revision onto which the Card editor explicitly follows untouched
fields. A rejected working copy stays in that editor; it is not an autosaved
session draft.

## The document write entry

| Writer | Client path | Transaction changes |
| --- | --- | --- |
| Mac definition JSON (`CardDatabaseView.apply`) | `RevisionEdit.apply` → `ProjectSession.applyEdit` | `setCardDefinition` → `PutDefinition` |
| Web definition JSON (`CardDatabaseView`, `apply`) | `RevisionEdit.apply` → `ProjectSession.applyEdit` | `setCardDefinition` → `PutDefinition` |
| Mac and Web Card settings (`CardEditorModel.apply`) | The same `RevisionEdit.apply` and `ProjectSession.applyEdit` | `setCardDefinition` → `PutDefinition`, plus source changes |
| Mac and Web comment text editors (`CommentsCategory`) | The same `RevisionEdit.apply` and `ProjectSession.applyEdit` | `setCommentText` → `setCardDefinition` → `PutDefinition` |
| Mac and Web general project settings, Nix sources and history setting | The same `RevisionEdit.apply` and `ProjectSession.applyEdit` | Project fields → `SetProject` |
| Immediate canvas edits, including additions, deletion, connections and inline values | `ProjectSession.edit` → `scheduleSave` → `saveOnce` | Session draft → project, definition, layout and source changes |
| Agent project tools | `tsugumi-agentd` `Host::State::ProjectCall` → shared C++ `client::Connection::Call`, with the tool's arguments | The same `ProjectSaveRequest` changes |

In each UI, `prepareSave` encodes the changes and `commitChanges` calls the
single `RPCClient.save` wrapper. On the Mac this wrapper is in
`macOS/Sources/RPC/Requests.swift`; on the Web it is in
`web/src/shared/rpc.ts`. Both send `ProjectSaveRequest.base_rev` and a fresh
`op_id`. Definition JSON uses the same `setCardDefinition` setter as the
ordinary Card editor. There is no separate settings-write RPC.

The canvas saver uses its draft base revision, follows notifications and
rebases its immediate edits before retrying a revision conflict. An editor
uses `applyEdit` instead: it flushes preceding immediate edits, checks its
own base, prepares the transaction through the same encoder, and submits it
once with that base. Preparing the transaction also records the undo step;
the step enters undo history only after a successful save. Source hashes
stay attached to the content read by the editor.

Top-level `tsugumi.json` and `definitions/<id>/_card.json`, `_meta.json` and
`_link.json` cannot be changed by `PutSource`, `PatchSource` or `RemoveSource`:
`Document::is_source_area` excludes them and `Store::stage` rejects them.
A Card in Card is a separate project copied under `externals/`; its document
files are source files of the containing project. The same transaction encoder
writes those files with both the containing project's revision and file hashes.
That is a different scope, not a second write entry for top-level definitions.
Project reload imports external disk edits through its revision-checked reload
operation; it is not an alternative JSON editor save.

## One exclusion check for people and the Agent

`Service::handle` dispatches `ProjectSaveRequest` onto the project operation
lane. `Service::save` checks `base_rev` against the store's current revision
before staging any changes. It returns `revision_conflict` and `current_rev`
without committing a stale transaction. This check applies to every origin.
The Agent's permissions checks follow it; neither Agent tool dispatch nor the
UI wrapper substitutes a newer revision for a submitted editor transaction.

If the Agent saves while a person edits, the Card editor takes in untouched
fields and lists fields changed on both sides. Apply cannot silently choose
those conflicts, including conflicts first discovered during Apply. The JSON
and project settings forms retain their opening revision. A newer revision
produces the same conflict choice in both UIs.

The person can choose **Overwrite**, or **Keep edits and reload**. Overwrite
submits against the revision for which the choice is shown. Another save after
that choice produces another conflict; overwrite is never an unconditional
write or an automatic retry. Reload displays the current content and retains
the previous text in a selectable disclosure so the person can reapply it.
JSON parsing and save failures retain the working text as well.

An Agent's sequence of saves is a sequence of transactions, not a lock spanning
its whole task. A person's apply can commit between two Agent saves. The
Agent's next save with an earlier base is then rejected by the same check and
must inspect the current document before deciding what to save next.
