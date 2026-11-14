# Run history and artifacts

A Card's name may change; its identifier does not. Renaming changes only the name shown in the app and to the Agent. Every past run record, target, input-source JSON, saved-file path and evidence link retains its original identifiers and bytes. Saved inputs remain usable for a partial run after a name change, including inside a Card in Card. Two Cards with the same name have distinct identifiers and distinct histories.

Open a run to see what its Cards received, produced and kept. A run can contain more than one invocation of a Card. The run record describes the execution, and the [version history](Versions.md) holds the saved project code and settings.

A **Partial run** (**部分実行**) consumes saved inputs when running one Card or from a Card downstream. It exists only for debugging, because AI-written code rarely works at once. Its result is not a result of the Workflow. Results computed from these values carry the mark downstream; a run consuming them downstream, or a later run consuming those saved values, also shows **Uses a partial run's result** (**部分実行の結果を使用**). Both marks can apply. Running the whole Workflow replaces the saved values and clears their provenance. **Record I/O** and **Retain artifact** are obeyed, and the kept values and artifacts retain the producing run's marks. The marks appear with runs, history, comparisons and kept results. Inputs always come from the last saved inputs; there is no choice of an earlier run.

The RPC exposes `RunStatus.partial_run` and `RunStatus.uses_partial_result` in status replies, `RunSummary.status` in run lists and comparisons, and run events. These flags are stored with the run and copied with history, independently of whether input/output bytes or artifacts are kept.

## Inspect an invocation

1. Open **Card settings → Run History** and choose a run. From the bottom timeline, select a Card to see its log and the stage it reached.
2. Select an invocation when the Card ran more than once. Open **Ports** to inspect recorded values, **Log** to read its output and diagnostics, or **Info** to see its status and execution details.
3. To keep values from future invocations, enable **Record I/O** under **Card settings → Ports** before running.

For example, open **Visualization → report**, run it, and inspect a figure Card's input and output. A stopped or failed invocation can have a log and status even when there is no output.

When a Card runs a batch, its history keeps each invocation separately, including which implementation and batch position it used. A failed batch retains the individual results and diagnostics in the history. The next Card receives a batch only after all its invocations succeed. [Run lists and Collect](features.md#run-lists-and-collect)

The **Info** tab shows the execution service recorded for a Code Card. For a remote job, it also shows the job ID, architecture, any requested CPU and memory limits, and the last reported state. The **Packages** tab shows the recorded Nix environments shared by the run’s Cards. [Run a Card on a remote worker](RemoteExecution.md)

## Compare two runs

1. Run a Workflow, change one condition, and run it again. For example, change the `offset` of the aggregate Card in **Visualization → report**.
2. Open **Compare Runs** from a Card's **Run History** or from the bottom run panel, then choose the two runs.
3. Compare a Card's state, duration, recorded execution conditions, parameters, values and retained display. If both runs have a saved version, open **Code and settings changes** to see which definitions changed.

Mac and Web show each Card's last invocation from each run in this view. For another invocation, open its individual record. The comparison marks missing values and Cards that did not run. A large value can still be compared by its saved type and hash when its contents are unavailable. If the comparison lacks enough evidence, it shows **Not comparable**. The comparison is read-only. Use **Version History** when you want to start again from an earlier version. [Version tree](Versions.md#the-version-tree)

## Find the runtime packages

Open **Card settings → Run History**, select the run and invocation, then open **Packages**. The run lists each recorded environment once; the invocation’s environment is selected initially. Choose an environment to see its target system, manifest output store path, derivation, and the Cards and invocation numbers sharing it. Environment entries and their users are paged.

Search for part of a package name, such as `python`, and expand a matching entry to select and copy its full Nix store path. The runtime closure is not a complete inventory of build-time dependencies. Search runs in the history database; **Previous** and **Next** display 100 matching paths at a time. For an invocation without a recorded environment, **Packages** reports that the record is unavailable.

**Compare Runs** marks Cards whose last invocation’s environment changed and lists **Removed runtime dependencies** on the left and **Added runtime dependencies** on the right. Both lists support search and paging. The core computes the exact difference between the two recorded closures in SQLite, so clients do not download every path. A missing environment reports that the comparison is unavailable rather than treating it as an empty closure.

Both UIs use `RunEnvironments`, `RunEnvironmentUses`, and `RunEnvironmentPaths`. These requests read recorded history only and never evaluate or build Nix. The records remain available after restarting, editing definitions, and importing history from another machine; they do not require the original store paths to exist locally.

The list uses the original history record. Configured cache endpoints, where recorded, describe which caches were available for builds. A path can come from a cache, the local store or a local build. [Binary cache provenance](BinaryCaches.md#storage-and-application)

## Keep and open artifacts

1. In **Card settings → Ports**, enable **Retain artifact** and choose a File or Directory output to keep.
2. Run the Card, open **Artifacts** in its settings, and select the saved file from that invocation. For example, open `index.html` from the **R reports** demo or inspect the saved data and metadata of a figure.
3. Open or export the saved member as needed. An artifact is a retained copy, so rerunning the Card or editing the source leaves the earlier copy available.

The Mac and Web HTML previews are static. They display embedded figures and styles with document scripts and external requests disabled. HTML up to 32 MiB can be previewed. Larger or script-dependent documents can be exported. [Report outputs and preview](Reports.md#rendering-and-saved-results)

## Follow evidence from chat

When the Agent cites a result in a chat reply, open its evidence link to inspect the referenced Card, run, invocation, log or artifact in that project. For example, ask which invocation produced a result and use the linked log to check it. A link becomes unavailable when the underlying record cannot be found in the project.

## Copy or delete history

With **Save the run history in the project** enabled, finished records, including the commit of the code that ran, are written to `.tsugumi/runs.sqlite` as well as to the local history. Copy the whole project, including its hidden `.tsugumi` directory, to keep its saved run records and files with it. The project's git history holds versions of the document and code. Outputs and logs live in run history.

If Tsugumi cannot open the project because its run history uses a different format, the startup page shows the versions involved. Choose **Archive History and Start** to keep the incompatible database files in a separate archive and open the project. If the archive is interrupted, choose **Resume Archiving and Start**. Records kept only in the archive are unavailable in the app. **Retry** attempts to open the project again without archiving. See the archive locations under [History storage](#history-storage).

Archiving leaves active remote jobs running on the execution host. Confirm they have finished or been cancelled before updating Tsugumi or archiving. The archive keeps the host history. Accepted remote job scratch stays available until the host durably records and acknowledges the outcome; acknowledgement removes inputs, outputs and logs, leaving replay tombstones. [Remote execution](RemoteExecution.md#journal-and-scratch-lifetime)

In **Settings → project → Run History**, choose the range and **Delete…** to remove its records and retained artifacts. Saved last inputs used for partial runs remain available. This action is unavailable during a run. **Delete** beside a project under **Build Cache** removes its build registrations and build work files. Invocation outputs and diagnostic work folders are separate scratch. Starting a run and reopening a project keep earlier scratch, including folders whose run record is missing. Under **Build Cache**, review **Past scratch in this project**: each folder (`runs/<run UUID>` or `recovery`) and its size are listed; unreadable sizes are explicitly unknown and do not prevent deletion. **Delete listed scratch…** removes only the folders shown when you choose the action. The latest run started on this machine has its scratch kept available for reruns and is excluded; deleting scratch is unavailable during a run or while external results are pending. Run History deletion also removes scratch belonging to the selected runs. Local invocation scratch is deleted only by these explicit actions. Recorded File / Directory values, artifacts and saved last inputs have independent copies; deleting scratch does not remove them. Recorded file values show the current kept-file location alongside the original value, or an explicit missing-content message. The original recorded bytes and hash stay unchanged when a project is copied. Artifact previews and downloads read retained copies and explicitly report missing content (including an incomplete project copy). Displays are stored in SQLite and do not depend on scratch. [History storage](#history-storage) · [Build and scratch space](Usage.md#manage-build-and-scratch-space)

## History storage

Archives contain the database and its SQLite sidecars together under `history-archive-<UUID>` beside the database. On Mac, the primary archive is on the VM history disk; for a directly run daemon it is in the project state directory. The portable archive is in the project's `.tsugumi/`.

[Storage paths and deletion controls](security.md#8-where-data-is-kept-and-how-to-delete-it).
