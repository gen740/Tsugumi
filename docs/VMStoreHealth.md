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
and launches during that update. Ordinary UI quit leaves ongoing work until
it finishes. After 30 seconds without Host clients or Agent work, hostd requests
an atomic `IdleShutdown` from each project core. The core accepts only with no
other connections, active run or pending/running request. Once no environment
is running and no client connects for 30 seconds, hostd and vmhost exit.

Vmhost gives the guest 40 seconds, then forces a stop; its second signal forces
immediately. A 42-second process deadline also bounds a stuck framework callback.
Hostd has a 45-second deadline. Vmd stops project groups concurrently, syncs,
and tries read-only remounts of the history disk, Nix overlay and store disk.
Only the successful path clears and syncs `/persist/store-health/running`.
A failed remount keeps the marker. SIGKILL, power loss and an expired shutdown
deadline cannot be made orderly; they cause a content check on the next boot.

`fsync-store-paths = true` and `fsync-metadata = true` address the separate window
between Nix registering a path and its file data reaching persistent storage.
The persistent private remote-worker store and image-builder store use the same
settings. This protects durable registration; it does not restore already
corrupt paths or repair physical storage failure.

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
TSUGUMI_VM_BUILD=1 macOS/Container/build-image.sh --vm
TSUGUMI_CODESIGN_IDENTITY="Apple Development" macOS/build-app.sh
```

The first command writes `dist/tsugumi-vm`; the second bundles it and the changed
Tsugumi source. `SharedVM.installImage` replaces the copied boot image when its
`vm-version` changes, retaining `store.img` and `history.img`. The guest rebuilds
vm-system from the bundled source hash. For a development session that still
runs the **old** helper, use the existing runtime Shutdown (or vmctl `quit`)
first; do not kill that old helper directly. Then end its idle host service and
launch the rebuilt app in the background. The new image's missing health version
forces the one-time check of the currently corrupt development store. No real
VM was started or repaired as part of this implementation.

## Focused verification and real-VM handoff

Without a VM: `tests/swift/vm_shutdown_test.swift` covers graceful coalescing,
repeated signals, deadline forcing and restart; `tsugumi_vm_lifetime_tests`
covers atomic idle stopping, another client, and queued versus running work;
`tests/python/test_vm_store_health.py` covers boot decisions and repair failure;
the focused cleanup test checks that teardown sends TERM only once and permits
the guest's deadline.

On a disposable real VM, verify TERM/INT/HUP during startup and normal operation,
Watch EOF after killing hostd, second-signal forcing, and the deadline with an
unresponsive guest. Confirm normal UI quit preserves a running Card until it
finishes, another client keeps the environment alive, and idle termination is
followed by a successful reconnect. Exercise an app update. Check read-only
remount results on both persistent disks and that a normal reboot skips checking.
Force an unclean stop and confirm checking precedes project readiness. Corrupt
a disposable cached path, observe successful repair and an unavailable repair,
and confirm the explicit Settings action reports the paths. Finally run the new
image once on the existing development disk and inspect the actual repair log.
