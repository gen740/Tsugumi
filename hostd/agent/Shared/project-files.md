# Project and Card files

Read with `demo_read reference/project-files.md`. Card examples and Draft implementation: `demo_read reference/cards.md`.

## _card.json

Write each _card.json with type and symbol ("none" shows the UI's own icon for the kind; write "none" unless the
person chose an icon).

- Code also requires inputs (initially []), maxRuns (1), reuseWorker (false), timeoutSeconds (0),
  buildTimeoutSeconds (0), recordLogs ("project"), recordIO ("on" in a Code Card you create or copy), sandbox (filesystem, pid, ipc and identity true,
  limits with cpu and memoryMiB both 0), grants (network and gui both false), systems (the Nix systems the Card works
  on, at least one; initially the host's native Linux system). A worker whose native system is listed builds and
  runs the Card natively; any other worker runs the first entry emulated.
- grants.hostPaths and secrets are optional. Each host path grant supplies name and write. Declare each name with its
  description in the containing tsugumi.json's optional hostPaths array. Hostd stores the user's folder binding per
  machine. If a run reports an unbound host path, ask the user to choose its folder in Project Settings > Host Paths.
- Merge also requires inputs (at least one, typically a and b), maxRuns (1) and recordIO ("project").
- Collect also requires maxRuns (1) and recordIO ("project").
- Draft, Trigger and Subgraph contain only type and symbol.

Zero timeouts or sandbox limits mean unlimited. Record settings and the optional Code setting pushOutputs inherit the
project with "project" and override with "on" or "off". pushOutputs controls publishing built Nix outputs. A Code Card may declare a probe derivation from
default.nix. The worker runs bin/probe in the Card sandbox. Its last line of output ranks eligible workers. The
highest positive score wins. Invocation history includes the scores and output.

## tsugumi.json

A new tsugumi.json carries formatVersion (1, the project file format), name, nixPath ([] or declared sources),
trackedPaths ([]), agentExclude (the host's defaults), recordLogs (true), recordIO (false), logLimitBytes
(134217728), outputLimitBytes (68719476736), outputLimitEntries (1000) and canvas (cards and connections). The
optional pushOutputs defaults to true. trackedPaths adds files or folders to Tsugumi's project versions.

## Code Card sources

Tsugumi generates _inputs.fbs for Code Cards, including an empty Inputs table when there are no connections. Pass
inputs = ./_inputs.fbs; to language SDK builders. Declare outputs in _outputs.fbs and use (required) for outputs every
call must emit. Edit the literal in params.nix to change a build-time value; a Card without params has no params.nix.

A Code Card's default.nix receives { params, inputs }, where inputs contains upstream Nix exports by Card id within
the same project level. A subgraph upstream appears under its own id with the merged exports of the inner Cards wired
to its outputs; Merge passes on the exports all its inputs share, Collect those of its source, and feedback wires and
Drafts nothing. Use Nix exports for pinned build inputs, hostPaths for local folders, and Artifacts for files produced
by a run.

## Nix sources and evaluation

Declare nixPath names and URLs in tsugumi.json, then use nix_path_lock_update for one name or all names. Each
external has its own declarations and lock.

evaluate_nix evaluates in the saved Card's directory for the local system, with params and inputs in scope. Use
(import ./. { inherit params inputs; }).api with build: true to realise the API derivation and read its files under
share/tsugumi/api/. See the SDK guide.

## Card in Card

A Card in Card has type "subgraph" and its own project at externals/<id>/. Its tsugumi.json may include a boundary
with inputs and outputs, both lists of {name, card, port}. Use project_edit on files under that directory. To rename
a subgraph id, move its _card.json and update the canvas references in one project_edit. The Host moves its
definition files and child project together.
