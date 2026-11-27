# Versions (git)

A project's folder is a git repository. Projects made in the app, and copies of demos, become repositories the first time they are opened. Other projects can be made repositories with "Start Versioning" in "Version History".

Versions contain the project's definitions: `tsugumi.json`, `tsugumi.lock`, `layout.bin`, canvas notes in `notes/<id>.md`, each Card's documents (`_card.json`, `README.md`, `_comments.json`) and source files, including `default.nix`, `params.nix` and generated `_inputs.fbs`, Skills (`skills/`), and embedded projects (`externals/`). The document model's source areas cover all files under each Card's `definitions/<id>/` folder, `skills/` and `externals/`, as in the project file APIs and saves.

These settings control Tsugumi's own project versions. They do not change Git tracking in your repository. Other files, including results and downloads outside those areas, enter a version only when explicitly listed in `trackedPaths` in `tsugumi.json`. Edit **Additional paths to version** in the project's General settings in either the Mac app or Web UI: one project-relative file or folder per line. Folder entries include their descendants. Paths are literal, without wildcard expansion; missing paths add nothing. For example:

```json
"trackedPaths": ["inputs/reference.csv", "calibration"]
```

Choosing data here fixes it as part of the computation's definition. Removing a path stops versioning it unless it is also in a definition's source area. Files outside this selection remain untracked and untouched, including files previously staged or tracked that the next version leaves out; their contents stay on disk. `.tsugumi/` and `.git/` at any depth are always excluded, regardless of `.gitignore` or the selection. Symbolic links are not followed or versioned. Definitions and explicit selections are included even if `.gitignore` ignores them.

## When a version is made

- **When a run starts**: if the saved definitions or additional selected paths have changed since the last version, the core commits them. The host displays the run message in the requested language. The run records the commit at its start, which you can see in the list of runs and in `RunStatus.commit`. Running again without changing them adds a run to the same commit (commits and runs are 1 : N). Unselected files do not mark the version history as changed.
- **Keep as Version**: commits the same definitions and additional selected paths and gives the version a name (an annotated tag) and a memo. Starting versioning uses the same selection.

## The version tree

In the Mac app, open it with "Version History…" (⌥⌘Y) in the Workflow menu; in the Web UI, with the button at the bottom of the sidebar.

- Branches and merges are drawn as lines, and each version shows the number and results of its runs (the dot's colour: green succeeded, red failed), the name of a version kept by hand (a tag), branch names and the Cards that changed.
- Hover a version's dot or row to see a short note on what changed in that version and a minimap of that version's Cards and connections, in which the Cards that changed in that version are highlighted.
- Select a version to see its message, its note and its runs' results and summaries. "Start from This Version" keeps the current definitions and additional selected paths as a version and then returns to that version's content.
- Returning to the version at the tip of a branch continues that branch. Changing and running from any other version starts a new branch (`try-<version>`); the earlier line is not lost.
- You cannot move to another version while there are unsaved changes or a run is going.

## Run summaries and version notes

Version History shows run summaries and a list of changed Cards. For values and logs, open [Run History](RunHistory.md). Ask the Agent to find a past trial by naming the changed Card and the result you remember. [Version records](design/VersionRecords.md) describes what the summaries and trial search use.

## Comments

A Card's comments ([Generating Cards from comments](Usage.md#working-with-the-agent)) are in `definitions/<id>/_comments.json`, so they are committed like the other settings. The details of a version in the version history show how the comments changed since the previous version (`VersionCommit.comments`).

- The changes are of five kinds: added, edited, resolved, reopened and deleted. One written and resolved within the same version counts as resolved.
- Each shows who did it (the Agent or the user) and, for a resolving save, the Cards it changed. Who deleted a comment goes with the comment, so it is not shown for deletions (an Agent's deletion remains in the chat's row).
- A comment keeps its id across versions.
- A version is made when a run starts or with "Keep as Version". A comment written and deleted in between is not kept; to keep one, resolve it instead of deleting it.
- They can be read from git too (for example `git show <version>:definitions/<id>/_comments.json`).

## Nix source pins

A version includes `tsugumi.json` and `tsugumi.lock` for the root project and its externals. Checking out a version restores its declarations and exact source pins. An explicit lock update reconciles one declaration or all declarations in the selected project. [Project Nix search path](design/Nix.md#project-nix-search-path)

## Demo histories

A demo folder's `.tsugumi-history` becomes commits, branches, tags and notes when a copy becomes a repository (the file itself goes away). The Cas A demo carries the process of trying bands, grids and stretches: failed branches, branches not adopted, the adopted version `rgb-v1`, the version that added the spectrum on top of it, and the version that pinned CIAO.

A demo history is the record of what ran. Its earlier versions keep the Card sources and ABI of the original trials. You can still inspect those sources in Git. Tsugumi rejects checkout of versions that use a removed Card type, and versions that use an older ABI may fail to build with the current SDKs. Use the current definitions to run the demo.
