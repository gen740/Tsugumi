# Run history and artifacts

Open a run to see what its Cards received, produced and kept. A run can contain more than one invocation of a Card. The run record describes the execution, and the [version history](Versions.md) holds the saved project code and settings.

## Inspect an invocation

1. Open **Card settings → Run History** and choose a run. From the bottom timeline, select a Card to see its log and the stage it reached.
2. Select an invocation when the Card ran more than once. Open **Ports** to inspect recorded values, **Log** to read its output and diagnostics, or **Info** to see its status and execution details.
3. To keep values from future invocations, enable **Record I/O** under **Card settings → Ports** before running.

For example, open **Visualization → report**, run it, and inspect a figure Card's input and output. A stopped or failed invocation can have a log and status even when there is no output.

The **Info** tab shows the execution service used by a Code Card once its backend starts. The **Packages** tab shows the recorded Nix system. [Execution service](features.md#execution-service)

## Compare two runs

1. Run a Workflow, change one condition, and run it again. For example, change the `offset` of the aggregate Card in **Visualization → report**.
2. Open **Compare Runs** from a Card's **Run History** or from the bottom run panel, then choose the two runs.
3. Compare a Card's state, duration, recorded execution conditions, parameters, values and retained display. If both runs have a saved version, open **Code and settings changes** to see which definitions changed.

Mac and Web show each Card's last invocation from each run in this view. For another invocation, open its individual record. The comparison marks missing values and Cards that did not run. A large value can still be compared by its saved type and hash when its contents are unavailable. If the comparison lacks enough evidence, it shows **Not comparable**. The comparison is read-only. Use **Version History** when you want to start again from an earlier version. [Version tree](Versions.md#the-version-tree)

## Find the runtime packages

Open **Card settings → Run History**, select the run and invocation, then open **Packages**. Search for part of a package name, such as `python`, and expand a matching entry to inspect its full Nix store path. The record shows the system and the runtime paths associated with that invocation. For an invocation without a recorded environment, **Packages** reports that the record is unavailable.

The list uses the original history record. Configured cache endpoints, where recorded, describe which caches were available for builds. A path can come from a cache, the local store or a local build. [Binary cache provenance](BinaryCaches.md#storage-and-application)

## Keep and open artifacts

1. In **Card settings → Ports**, enable **Retain artifact** and choose a File or Directory output to keep.
2. Run the Card, open **Artifacts** in its settings, and select the saved file from that invocation. For example, open `index.html` from the **R reports** demo or inspect the saved data and metadata of a figure.
3. Open or export the saved member as needed. An artifact is a retained copy, so rerunning the Card or editing the source leaves the earlier copy available.

The Mac and Web HTML previews are static. They display embedded figures and styles with document scripts and external requests disabled. HTML up to 32 MiB can be previewed. Larger or script-dependent documents can be exported. [Report outputs and preview](Reports.md#rendering-and-saved-results)

## Follow evidence from chat

When the Agent cites a result in a chat reply, open its evidence link to inspect the referenced Card, run, invocation, log or artifact in that project. For example, ask which invocation produced a result and use the linked log to check it. A link becomes unavailable when the underlying record cannot be found in the project.

## Copy or delete history

With **Save run history in the project** enabled, finished records are written to `.tsugumi/runs.sqlite` as well as to the local history. Copy the whole project, including its hidden `.tsugumi` directory, to keep its saved run records and files with it. The project's git history holds versions of the document and code. Outputs and logs live in run history.

**Delete Run History** in the project settings deletes the selected history and retained artifacts. Saved last inputs used for partial runs remain available. This action is unavailable during a run. **Delete the build cache** operates on build registrations and work files. [History storage](features.md#6-run-history-and-artifacts) · [Build results](features.md#keeping-build-results)
