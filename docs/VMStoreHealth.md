# Shared VM store integrity

A development VM content check found 14 paths that Nix still registered as valid
but whose files were empty or truncated at multiples of 4 MiB. The resident Mac
VM helper previously died immediately on SIGTERM/SIGINT, including at logout and
restart. These are observed shutdown and corruption cases, rather than a
hypothetical cache problem.

The protection assumes the host can deliver a termination signal and allow the
guest time to stop. Hostd and vmhost now request orderly guest shutdown on TERM,
INT and HUP; loss of hostd's Watch connection also shuts down vmhost. They are
resident services, independent of the spawning UI's parent PID. An app update
ends hostd before replacing the app. Existing read observers do reconnect across
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

If a Web UI is open when the Mac app quits, its existing reconnect starts the host again.
That is treated as the Web UI using the host; reconnect behaviour is unchanged.

There is no idle self-exit. Without the Mac app, the CLI/Web server starts or
reuses the resident host on demand. Ending that client only disconnects; the
host stays until explicitly stopped (TERM/INT/HUP, or the existing update path).
A pending CLI/plug-in call loses its service connection when the Mac app quits;
its run is stopped, not handed over. A later client request may start a host
again. No new command-only lifetime manager is supplied.

Vmhost gives the guest 40 seconds, then forces a stop. Vmd stops project groups concurrently and ends the Nix daemons. Once all writers have stopped, it flushes the backing store disk with `sync -f /persist`, clears `/persist/store-health/running`, and syncs that unlink. The store disk stays writable because its mounted overlay holds the upper/work directories open. A failed flush or a surviving daemon retains the dirty marker for the next boot.

Boot writes and syncs the marker before store registration. A dirty marker,
a missing health version (including **existing disks on the first new image
boot**), or a changed boot closure runs root's
`nix-store --verify --check-contents --repair` before vm-system or projects start.
An unsuccessful repair blocks startup. The environment status explains the check
in Japanese and English. The VM console and `/persist/store-health/check.log`
contain repaired paths and failures. Settings → execution environment data also
has an explicit check/repair action; it stops project environments first.
No run history or artifacts are deleted by these checks.

## Picking up the image change

Rebuild the boot image; rebuilding only the C++/Swift source is insufficient:

```sh
TSUGUMI_VM_BUILD=1 vm/guest/build-image.sh --vm
TSUGUMI_CODESIGN_IDENTITY="Apple Development" ui/macos/build-app.sh
```

The first command writes the verified artifact set to `dist/tsugumi-vm`; the
second bundles its single guest archive and manifest. `SharedVM.installImage`
extracts the kernel/rootfs in private support when the manifest identity changes
or extracted contents are incomplete, retaining `store.img`.
The image already contains vm-system; the guest starts it after store checks. For a development session that still
runs the **old** helper, use the existing runtime Shutdown (or vmctl `quit`)
first; do not kill that old helper directly. Then end its host service and
launch the rebuilt app in the background. The new image's missing health version
forces the one-time check of the currently corrupt development store. No real
VM was started or repaired as part of this implementation.

## Focused verification and real-VM handoff

Without a VM: `tests/swift/vm_shutdown_test.swift` covers graceful coalescing,
repeated signals, deadline forcing and restart; `tsugumi_vm_lifetime_tests`
covers activity observation with another client and queued versus running work;
`tests/swift/app_termination_test.swift` covers quit/cancel, idle quit, duplicate
termination requests and bounded cancellation waits;
`tests/python/test_vm_store_health.py` covers boot decisions, the actual
poweroff path with a busy backing filesystem, failed remount/sync and repair failure;
the focused cleanup test checks that teardown sends TERM only once and permits
the guest's deadline.

On a disposable real VM, verify TERM/INT/HUP during startup and normal operation,
Watch EOF after killing hostd, second-signal forcing, and the deadline with an
unresponsive guest. Confirm Quit asks about a running Card, an Agent turn and a save (also from
another client); Cancel keeps them alive and Quit and Stop ends hostd and the VM.
Confirm idle Quit asks nothing, both last-window settings work, and a later
explicit client request can start a host again. Exercise an app update. Check the history/overlay
read-only remounts and backing store sync, and that a normal reboot skips checking.
Force an unclean stop and confirm checking precedes project readiness. Corrupt
a disposable cached path, observe successful repair and an unavailable repair,
and confirm the explicit Settings action reports the paths. Finally run the new
image once on the existing development disk and inspect the actual repair log.
