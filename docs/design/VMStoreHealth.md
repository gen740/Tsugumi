# Shared VM lifecycle and store integrity

## Host and guest shutdown

Hostd and vmhost request orderly guest shutdown on TERM,
INT and HUP; loss of hostd's Watch connection also shuts down vmhost. They are
resident services, independent of the spawning UI's parent PID. An app update
ends hostd before replacing the app. Read observers reconnect across
host restarts, so an exiting client process also blocks further host connections
and launches during that update.

On the Mac, Quit requests the same orderly host shutdown even when other clients
are connected. The app waits at most two seconds for host exit; hostd and vmhost
finish their bounded shutdown independently. Before quitting, the app observes
host Agent turns and queued/running requests, and asks each running project core
about runs and requests (including saves from other connections). Ongoing work
gets one Quit and Stop / Cancel alert; idle connections alone do not. A failed
activity query also asks, because it cannot establish that other clients are idle.
The observation is a snapshot: it does not reserve or transfer run ownership.
General Settings can make closing the last window quit the app; the default is
off, so closing all windows keeps the app and VM alive.

A Web UI left open can restart the host during reconnect.

There is no idle self-exit. Without the Mac app, `tsugumi web` starts or
reuses the resident host on demand. Ending that client only disconnects; the
host stays until explicitly stopped (TERM/INT/HUP, or the existing update path).
A pending CLI/plug-in call loses its service connection when the Mac app quits;
its run is stopped, not handed over. A later client request may start a host
again.

Vmhost waits for the guest to power off. A repeated stop signal forces it off. Vmd stops project groups concurrently and ends the Nix daemons. Once all writers have stopped, it flushes the backing store disk with `sync -f /persist`, clears `/persist/store-health/running`, and syncs that unlink. The store disk stays writable because its mounted overlay holds the upper/work directories open. A failed flush or a surviving daemon retains the dirty marker for the next boot.

Boot writes and syncs the marker before store registration. A dirty marker,
a missing health version,
or a changed boot closure runs root's
`nix-store --verify --check-contents --repair` before vm-system or projects start.
An unsuccessful repair blocks startup. The environment status explains the check
in Japanese and English. The VM console and `/persist/store-health/check.log`
contain repaired paths and failures. Settings → execution environment data also
has an explicit check/repair action; it stops project environments first.
No run history or artifacts are deleted by these checks.

## Project slots

The shared VM gives each running project environment one of `SharedVM.slotCount`
(16) virtiofs devices. An environment keeps its slot until it stops; closing a
window does not stop it. A project is held while a window or Web UI page shows
it (HostPageRequest) or while an Agent client's connection showed it last
(ShowRequest, until CloseRequest or the connection ends: `tsugumi_close` and the
end of an MCP session). When the VM refuses a start because every slot is taken
(`no_project_slot`), hostd stops the least recently used environment that
nothing holds and in which nothing runs (ProjectActivity), then starts again.
When none qualifies, the start fails with `NoProjectSlot`: an English message
and `slot_holders` (each project with page, session or busy), which the UIs
show in their own words. Stopping keeps the project's documents and run history.

## Guest image

The guest boots a release-built Linux kernel and EROFS root filesystem.
The kernel, root filesystem and identity are bundled in one archive. The
helper extracts the attachments into its private support folder and reuses
them when the image identity matches and both files exist. An image change
keeps the persistent store disk and triggers the store check above.

Boot overlays the persistent store on the read-only image closure, registers
the shipped store paths and starts the prebuilt guest supervisor. The Mac
build consumes a verified image artifact. See [release procedures](../../RELEASING.md)
and [development builds](../INSTALL.md#building-from-source).

## Verification

`tests/swift/vm_shutdown_test.swift` covers shutdown coalescing and deadlines.
`tests/swift/app_termination_test.swift` covers quit, cancel and active work.
`tests/static/test_vm_store_health.py` covers boot decisions, failed sync and
repair, and shutdown with a busy store filesystem. Use the [VM tests](../../tests/README.md)
to exercise the supplied image and native helper together.
