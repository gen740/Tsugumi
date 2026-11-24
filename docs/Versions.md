# Versions (git)

A project's folder is a git repository. Projects made in the app, and copies of demos, become repositories the first time they are opened. Other projects can be made repositories with "Start Versioning" in "Version History".

Versions contain the project's definitions: `tsugumi.json`, `tsugumi.lock`, `layout.bin`, canvas notes in `notes/<id>.md`, each Card's documents (`_card.json`, `README.md`, `_comments.json`) and source files, including `default.nix`, `params.nix` and generated `_inputs.fbs`, Skills (`skills/`), and embedded projects (`externals/`). The document model's source areas cover all files under each Card's `definitions/<id>/` folder, `skills/` and `externals/`, as in the project file APIs and saves.

These settings control Tsugumi's own project versions. They do not change Git tracking in your repository. Other files, including results and downloads outside those areas, enter a version only when explicitly listed in `trackedPaths` in `tsugumi.json`. Edit **Additional paths to version** in the project's General settings in either the Mac app or Web UI: one project-relative file or folder per line. Folder entries include their descendants. Paths are literal, without wildcard expansion; missing paths add nothing. For example:

```json
"trackedPaths": ["inputs/reference.csv", "calibration"]
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

A version's note is its change record. The project core compares the full version tree with its first parent, without a model. It lists Cards added or removed and changes to their definitions, source, parameters and wiring, as neutral kinds of change. The host renders these facts in the requesting app's language. Workflow settings are listed separately. The host shows at most six entries and the remaining count; the record keeps all entries. It contains no source text or parameter values. All files under a Card's `src/` count as source inputs, including assets. Changes to `layout.bin` and files under `data/` are excluded.

The project RPC returns typed commit, run and change facts. Its binary Git note format stays private to the project daemon; the host formats the public facts for both clients and tools. The clients request the note when a version is hovered or selected, and display the returned text. No model is called, so several versions can be browsed freely.

The core stores the language-neutral record in `refs/notes/tsugumi-record`: added and removed Card IDs, changed Card IDs with change kinds, and whether Workflow settings changed. It contains no sentence templates.

To learn what a change did in more detail, ask the Agent. It reads the version's diff with `version_diff` (in the `versions` tool family) and answers from it. The diff covers `tsugumi.json` and regular text files anywhere in a Card definition. Files in `data/`, CSV files, symbolic links, files over 1 MiB and files with a NUL byte in the first 8 KiB are excluded. `layout.bin` is also excluded. Long diffs are cut (4,000 bytes per file, 12,000 in all). Parameter values and values written in code are included. `HostVersionDiffRequest` provides the same diff to run comparisons. Read the record from git with:

```bash
git log --notes=tsugumi-record
```

### Finding a past trial

A person may remember a trial but not its run: "the run where I changed the background correction and the peak disappeared". The Agent's `version_find` (the `versions` family's `find`) narrows the recorded history to candidates. It reads no new data and stores nothing; it composes the reads the Agent could make one at a time:

1. `version_graph`: the versions (newest first, up to 500) whose changed Cards include the named Card or a Card inside it, and the runs recorded for each.
2. The change record above: how the Card changed (`source`, `parameters`, `definition`, `wiring`, `added`, `removed`); `kinds` narrows to some of them.
3. `run_record` (latest invocation) of the observed Card, by default the changed Card, in each run of a candidate and in the newest run of the nearest earlier version that recorded it.

Each output port is reported as `same`, `changed` or `unknown` against that earlier run, by kind, type and recorded content hash (a File's or Directory's own content hash). A missing hash, a deleted run, a Card that did not run, or no earlier run gives `unknown` or an `unverified` note, never `same`. The result carries `card:`, `run:<id>` and `run:<id>/<card>#<invocation>` links for the Agent to cite. A different hash says only that the content differs: whether "the peak disappeared" is read from the values (`run_record`, `run_value_read`). Candidates default to 10 versions (at most 30), and one call reads at most 40 records; the rest are left for `run_record`.

## Comments

A Card's comments ([Generating Cards from comments](Usage.md#working-with-the-agent)) are in `definitions/<id>/_comments.json`, so they are committed like the other settings. The details of a version in the version history show how the comments changed since the previous version (`VersionCommit.comments`).

- The changes are of five kinds: added, edited, resolved, reopened and deleted. One written and resolved within the same version counts as resolved.
- Each shows who did it (the Agent or the user) and, for a resolving save, the Cards it changed. Who deleted a comment goes with the comment, so it is not shown for deletions (an Agent's deletion remains in the chat's row).
- A comment keeps its id across versions.
- A version is made when a run starts or with "Keep as Version". A comment written and deleted in between is not kept; to keep one, resolve it instead of deleting it.
- They can be read from git too (for example `git show <version>:definitions/<id>/_comments.json`).

## Nix source pins

A version includes `tsugumi.json` and `tsugumi.lock` for the root project and its externals. Checking out a version restores its declarations and exact source pins. An explicit lock update reconciles one declaration or all declarations in the selected project. [Project Nix search path](features.md#project-nix-search-path)

## Demo histories

A demo folder's `.tsugumi-history` becomes commits, branches, tags and notes when a copy becomes a repository (the file itself goes away). The Cas A demo carries the process of trying bands, grids and stretches: failed branches, branches not adopted, the adopted version `rgb-v1`, the version that added the spectrum on top of it, and the version that pinned CIAO.

A demo history is the record of what ran. Its earlier versions keep the Card sources of the time they were made, written for the Card ABI of that time, so they show how the analysis went but may not build with the current SDKs. The demo's current definitions are the runnable ones.
