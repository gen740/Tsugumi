# Versions (git)

A project's folder is a git repository. Projects made in the app, and copies of demos, become repositories the first time they are opened. Other projects can be made repositories with "Start Versioning" in "Version History". `.tsugumi/` (tsugumid's state) is excluded in `.gitignore`.

## When a version is made

- **When a run starts**: if the content has changed since the last version, the saved content is committed (message "Run: <Card>" in English, "実行: <Card>" in Japanese). The run records the commit at its start, which you can see in the list of runs and in `RunStatus.commit`. Running again without changing anything adds a run to the same commit (commits and runs are 1 : N).
- **Keep as Version**: commits the current content and gives it a name (an annotated tag) and a memo.

## The results of runs

Each time a run ends, one line is appended to the note of that run's commit (`refs/notes/tsugumi`). The end of the line is a short summary of what ran:

```
run 3f2a9c1e-… succeeded 2026-11-04T10:12:00Z 12.3s whole: load, bands ×3, rgb
run 7c01d2aa-… failed 2026-11-04T10:15:00Z 1.8s from bands (with saved inputs): bands / failed: bands (worker_failed) / not run: rgb
```

The commit message and the summary are in the language of the app (or Web UI) that started the run: its client tells tsugumid in `SessionHello` (`language`), so a model cannot change it. In Japanese the same lines read `全体: load、bands ×3、rgb` and `bands から（保存済みの入力で）: bands / 失敗: bands（worker_failed） / 未実行: rgb`.

The summary is made mechanically from the results the core recorded for that run. No model is used.

- **Where it started**: `whole` (a run from a trigger or a Workflow), `from <Card>` (that Card and downstream), `only <Card>`. When any Card that ran used saved inputs from an earlier run, `(with saved inputs)` is added.
- **Cards that ran**: the Cards actually invoked. One invoked several times gets `×count`. The trigger of a whole run is not included.
- **Failed**: the Cards that failed and the kind of failure (the kind of `Failure`). The failure's text and logs are not put in git; see them in the run's record.
- **Not run**: Cards not invoked because something upstream failed or stopped.
- To keep it short, each list has at most six Cards, and the rest are `and N more`.

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
- Hover a version's dot or row to see a short sentence on what changed in that version (the version summary) and a minimap of that version's Cards and connections, in which the Cards that changed in that version are highlighted.
- Select a version to see its message, its version summary and its runs' results and summaries. "Start from This Version" keeps the current content as a version and then returns to that version's content.
- Returning to the version at the tip of a branch continues that branch. Changing and running from any other version starts a new branch (`try-<version>`); the earlier line is not lost.
- You cannot move to another version while there are unsaved changes or a run is going.

## Version summaries

Turn on "Version summaries" above the version history (it asks first; off by default) and a small model (`claude-haiku-5-5` for Anthropic; for OpenAI, the Agent's model) writes a short sentence on what changed in each version. This is different from the run summary: it is text a model writes.

- **When it is made**: once, when a run of that version ends, or when you hover a version without a summary. The host-side Agent service reads the authorized version diff and requests the model summary; `tsugumid` never calls a model. For runs while no page or app is open, it is made the next time you hover.
- **What is sent**: that version's `VersionDiff`, limited to diffs of `tsugumi.json`, the Cards' `_card.json`, `_meta.json`, `_link.json`, `_outputs.fbs` and `default.nix`, and the code in `src/` (`.py` `.cc` `.cpp` `.cxx` `.c` `.h` `.hh` `.hpp` `.nix` `.fbs` `.sh`). Data files such as `data/`, non-code files in `src/` and `layout.json` are not sent. Long diffs are cut (4,000 bytes per file, 12,000 in all). Parameter values in `_card.json` and values written in code are sent, though.
- **Where it is kept**: a commit note (`refs/notes/tsugumi-summary`). It is kept apart from the run records (`refs/notes/tsugumi`), so `git log --notes=tsugumi --grep=failed` is not affected by the model's text. Read it from git with:

```bash
git log --notes=tsugumi-summary
```

- The model is asked to write only from the diff, but it is a model's text and can be wrong. What is certain is the diff and the run records.
- It is not made with the ChatGPT plan (the plan's allowance is for the Agent).

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
