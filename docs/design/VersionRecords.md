# Version storage and recovery

The project core stores language-neutral Git records. The host renders their summaries for the requesting client. See [Versions](../Versions.md) for version selection and the files included in a version.

## The results of runs

When a run ends, the core adds it to the commit's FlatBuffers note in
`refs/notes/tsugumi`. A run entry holds its ID, terminal state, end time,
duration and structured facts. The facts include the starting Card,
propagation and saved-input flags, invocation counts, failure kinds and
Cards that never ran. Values, failure details and logs stay in [run history](RunStorage.md#history-storage).

The core writes ordinary Git commit messages and exposes typed version facts
over Project RPC. User and imported demo messages remain opaque text.

The host service (`tsugumi-hostd`) turns these records into Japanese or English on each request, so the same saved commit and run can be read in either language later. The Mac app and Web UI use `HostVersionGraphRequest`; the Agent uses the same host presentation logic. For example, a run can read `whole: load, bands ×3, rgb` or `全体: load、bands ×3、rgb`. No model is used for this presentation.

- **Where it started**: `whole` (a run from a trigger or a Workflow), `from <Card>` (that Card and downstream), `only <Card>`. When any Card that ran used saved inputs from an earlier run, `(with saved inputs)` is added.
- **Cards that ran**: the Cards actually invoked. One invoked several times gets `×count`. The trigger of a whole run is not included.
- **Failed**: the Cards that failed and the kind of failure (the kind of `Failure`). The failure's text and logs are not put in git; see them in the run's record.
- **Not run**: Cards not invoked because something upstream failed or stopped.
- The host displays at most six Cards per list and the remaining count. The stored record retains every Card.

## Version notes

A version's note is its change record. The project core compares the full version tree with its first parent, without a model. It lists Cards added or removed and changes to their definitions, source, parameters and wiring, as neutral kinds of change. The host renders these facts in the requesting app's language. Workflow settings are listed separately. The host shows at most six entries and the remaining count; the record keeps all entries. It contains no source text or parameter values. All files under a Card's `src/` count as source inputs, including assets. Changes to `layout.bin` and files under `data/` are excluded.

The project RPC returns typed commit, run and change facts. Its binary Git note format stays private to the project daemon; the host formats the public facts for both clients and tools. The clients request the note when a version is hovered or selected, and display the returned text. No model is called, so several versions can be browsed freely.

The core stores the language-neutral record in `refs/notes/tsugumi-record`: added and removed Card IDs, changed Card IDs with change kinds, and whether Workflow settings changed. It contains no sentence templates.

To learn what a change did in more detail, ask the Agent. It reads the version's diff with `version_diff` (in the `versions` tool family) and answers from it. The diff covers `tsugumi.json` and regular text files anywhere in a Card definition. Files in `data/`, CSV files, symbolic links, files over 1 MiB and files with a NUL byte in the first 8 KiB are excluded. `layout.bin` is also excluded. Long diffs are cut (4,000 bytes per file, 12,000 in all). Parameter values and values written in code are included. `HostVersionDiffRequest` provides the same diff to run comparisons.

### Finding a past trial

A person may remember a trial but not its run: "the run where I changed the background correction and the peak disappeared". The Agent's `version_find` (the `versions` family's `find`) narrows the recorded history to candidates. It reads no new data and stores nothing; it composes the reads the Agent could make one at a time:

1. `version_graph`: the versions (newest first, up to 500) whose changed Cards include the named Card or a Card inside it, and the runs recorded for each.
2. The change record above: how the Card changed (`source`, `parameters`, `definition`, `wiring`, `added`, `removed`); `kinds` narrows to some of them.
3. `run_record` (latest invocation) of the observed Card, by default the changed Card, in each run of a candidate and in the newest run of the nearest earlier version that recorded it.

Each output port is reported as `same`, `changed` or `unknown` against that earlier run, by kind, type and recorded content hash (a File's or Directory's own content hash). A missing hash, a deleted run, a Card that did not run, or no earlier run gives `unknown` or an `unverified` note, never `same`. The result carries `card:`, `run:<id>` and `run:<id>/<card>#<invocation>` links for the Agent to cite. A different hash says only that the content differs: whether "the peak disappeared" is read from the values (`run_record`, `run_value_read`). Candidates default to 10 versions (at most 30), and one call reads at most 40 records; the rest are left for `run_record`.

## Working-tree status

Version Graph compares the version-selected files with HEAD to report dirty
state. It compares blob identities and paths without writing Git objects.

## Checkout journal and recovery

A checkout requires a saved, idle project. The core prepares and validates a
preview of the destination tree before replacing live project files. It
preserves the affected roots, Git index, HEAD and branch target in checkout
scratch. It writes a durable intent to `.tsugumi/checkout.json` before the
first live rename and clears it after the checkout commits.

On startup the core recovers a pending checkout before opening the project
document. It restores the original roots, index, HEAD and branch target.
Recovery can repeat if it is interrupted. Startup reports damaged or missing
recovery data instead of opening a partially replaced document.
