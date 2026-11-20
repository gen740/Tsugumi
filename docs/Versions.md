# Versions (git)

A project's folder is a git repository. Projects made in the app, and copies of demos, become repositories the first time they are opened. Other projects can be made repositories with "Start Versioning" in "Version History".

Versions contain the project's definitions: `tsugumi.json`, `layout.json`, each Card's documents (`_card.json`, `_meta.json`) and source files, including Nix definitions and pins, Skills (`skills/`), and embedded projects (`externals/`). The document model's source areas cover all files under each Card's `definitions/<id>/` folder, `skills/` and `externals/`, as in the project file APIs and saves.

Other files, including results and downloads outside those areas, enter a version only when explicitly listed in `versionPaths` in `tsugumi.json`. Edit **Additional paths to version** in the project's General settings in either the Mac app or Web UI: one project-relative file or folder per line. Folder entries include their descendants. Paths are literal, without wildcard expansion; missing paths add nothing. For example:

```json
"versionPaths": ["inputs/reference.csv", "calibration"]
```

Choosing data here fixes it as part of the computation's definition. Removing a path stops versioning it unless it is also in a definition's source area. Files outside this selection remain untracked and untouched, including files previously staged or tracked that the next version leaves out; their contents stay on disk. `.tsugumi/` and `.git/` at any depth are always excluded, regardless of `.gitignore` or the selection. Symbolic links are not followed or versioned. Definitions and explicit selections are included even if `.gitignore` ignores them.

## When a version is made

- **When a run starts**: if the saved definitions or additional selected paths have changed since the last version, they are committed (a language-neutral JSON commit record; the host displays "Run: <Card>" or "実行: <Card>"). The run records the commit at its start, which you can see in the list of runs and in `RunStatus.commit`. Running again without changing them adds a run to the same commit (commits and runs are 1 : N). Unselected files do not mark the version history as changed.
- **Keep as Version**: commits the same definitions and additional selected paths and gives the version a name (an annotated tag) and a memo. Starting versioning uses the same selection.

## The results of runs

Each time a run ends, one JSON record is appended to the note of that run's commit (`refs/notes/tsugumi`). It holds `run_id`, the terminal `state`, UTC `ended` time, `seconds`, and `record_json`: neutral facts about where the run started, whether it propagated or used saved inputs, the IDs and invocation counts of Cards that ran, failure kinds, and IDs of Cards never invoked. The schema is in `tsugumid/runtime/src/version_record.hh`. No failure messages, logs, display names or data values are stored in these notes.

Automatic commit messages are JSON records with `tsugumi: "commit"`, `kind` (`create`, `run`, `checkout`) and a Card ID. User-written messages and imported demo messages remain opaque text. Neither writing nor reading a project RPC selects a language; `SessionHello` has no language field.

The host service (`tsugumi-hostd`) turns these records into Japanese or English on each request, so the same saved commit and run can be read in either language later. The Mac app and Web UI use `HostVersionGraphRequest`; the Agent uses the same host presentation logic. For example, a run can read `whole: load, bands ×3, rgb` or `全体: load、bands ×3、rgb`. No model is used for this presentation.

- **Where it started**: `whole` (a run from a trigger or a Workflow), `from <Card>` (that Card and downstream), `only <Card>`. When any Card that ran used saved inputs from an earlier run, `(with saved inputs)` is added.
- **Cards that ran**: the Cards actually invoked. One invoked several times gets `×count`. The trigger of a whole run is not included.
- **Failed**: the Cards that failed and the kind of failure (the kind of `Failure`). The failure's text and logs are not put in git; see them in the run's record.
- **Not run**: Cards not invoked because something upstream failed or stopped.
- The host displays at most six Cards per list and the remaining count. The stored record retains every Card.

Runs of versions made from a demo's history have no summary (the history file has no such record).

Versions can be chosen by result from git too:

```bash
git log --notes=tsugumi --grep=failed
```

```bash
git tag -l
```

Outputs and logs are in [run history](RunHistory.md#history-storage), separate from git.

## The version tree

In the Mac app, open it with "Version History…" (⌥⌘Y) in the Workflow menu; in the Web UI, with the button at the bottom of the sidebar.

- Branches and merges are drawn as lines, and each version shows the number and results of its runs (the dot's colour: green succeeded, red failed), the name of a version kept by hand (a tag), branch names and the Cards that changed.
- Hover a version's dot or row to see a short note on what changed in that version and a minimap of that version's Cards and connections, in which the Cards that changed in that version are highlighted.
- Select a version to see its message, its note and its runs' results and summaries. "Start from This Version" keeps the current definitions and additional selected paths as a version and then returns to that version's content.
- Returning to the version at the tip of a branch continues that branch. Changing and running from any other version starts a new branch (`try-<version>`); the earlier line is not lost.
- You cannot move to another version while there are unsaved changes or a run is going.

## Version notes

Choose **Change record** or **Agent summary** beside the version history. This setting applies to all projects opened through the same hostd (`versions.noteMode`). Change record is the default.

- **Change record**: the project core compares the full version tree with its first parent, without a model. It lists Cards added or removed and changes to their definitions, source, parameters and wiring, as neutral kinds of change. The host renders these facts in the requesting app's language. Workflow settings are listed separately. The host shows at most six entries and the remaining count; the record keeps all entries. It contains no source text or parameter values. All files under a Card's `src/` count as source inputs, including assets and languages outside the model-diff allowlist. Changes to `layout.json` and files under `data/` are excluded.
- **Agent summary**: the Agent writes one short sentence from the code and settings diff. The diff goes to the model provider and uses tokens. It can contain mistakes; the diff and run records are the evidence. This is different from the mechanical run summary above.

The project RPC returns typed commit, run and change facts. Its Git note JSON format stays private to the project daemon; the host formats the public facts for both clients and tools.

The clients request a note when a version is hovered or selected, and display the returned text. The host Agent service chooses the mode and model, reads the authorized diff, generates the summary when needed and saves it. Requests from multiple windows for the same project and version share a gate: the saved Agent note is checked before any model call. A successful note is reused across windows and service restarts. Failed requests can be retried. Runs with no open version history do not request model summaries.

The core stores the language-neutral mechanical record in `refs/notes/tsugumi-record`: added and removed Card IDs, changed Card IDs with change kinds, and whether Workflow settings changed. It contains no sentence templates. The host renders it in the requesting app's language. Generated Agent notes are stored separately per language. Switching to Change record shows the mechanical record even if an Agent note is saved; switching back reuses the saved Agent note.

Agent summaries use the host's small-model override (`agent.thoughtModel` or `agent.thoughtModel.openai`) when set; the defaults are `claude-haiku-5-5` for Anthropic and the Agent's configured model for OpenAI. They are unavailable with the ChatGPT plan. If no code or settings changed, the mechanical record is returned without calling a model.

The host owns `SummaryReads`, the file allowlist and diff limits. The model receives `VersionDiff`, limited to diffs of `tsugumi.json`, the Cards' `_card.json`, `_meta.json`, `_outputs.fbs` and `default.nix`, and code in `src/` (`.py` `.cc` `.cpp` `.cxx` `.c` `.h` `.hh` `.hpp` `.nix` `.fbs` `.sh`). Data files such as `data/`, non-code files in `src/` and `layout.json` are excluded. Long diffs are cut (4,000 bytes per file, 12,000 in all). Parameter values and values written in code are sent.

The host writes Agent notes to `refs/notes/tsugumi-summary-ja` or `refs/notes/tsugumi-summary-en`, apart from the neutral core records. `HostVersionDiffRequest` provides the same host-selected diff to run comparisons. Read it from git with:

```bash
git log --notes=tsugumi-summary-en
git log --notes=tsugumi-record
```

## Comments

A Card's comments ([Generating Cards from comments](Usage.md#working-with-the-agent)) are in `definitions/<id>/_meta.json`, so they are committed like the other settings. The details of a version in the version history show how the comments changed since the previous version (`VersionCommit.comments`).

- The changes are of five kinds: added, edited, resolved, reopened and deleted. One written and resolved within the same version counts as resolved.
- Each shows who did it (the Agent or the user) and, for a resolving save, the Cards it changed. Who deleted a comment goes with the comment, so it is not shown for deletions (an Agent's deletion remains in the chat's row).
- Comments are told apart by id, so renaming a Card does not make them different comments.
- A version is made when a run starts or with "Keep as Version". A comment written and deleted in between is not kept; to keep one, resolve it instead of deleting it.
- They can be read from git too (for example `git show <version>:definitions/<id>/_meta.json`).

## nixpkgs

A new project holds the nixpkgs of the Tsugumi that made it (`flake.lock`) as a fixed value in `pkgsPath` of `tsugumi.json`. It is part of the version, so going back to an earlier version brings back its nixpkgs too. A project that is not pinned (`tsugumi:default`) can be pinned in the Workflow settings.

## Demo histories

A demo folder's `.tsugumi-history.json` becomes commits, branches, tags and notes when a copy becomes a repository (the file itself goes away). The Cas A demo carries the process of trying bands, grids and stretches: failed branches, branches not adopted, the adopted version `rgb-v1`, the version that added the spectrum on top of it, and the version that pinned CIAO.
