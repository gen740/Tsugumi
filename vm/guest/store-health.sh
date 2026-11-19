# Run as root with the boot image's tools on PATH. No projects may be running.
set -euo pipefail
action=$1
persist=$2
health="$persist/store-health"
mkdir -p "$health"
chmod 700 "$health"

case "$action" in
  begin)
    reason=""
    if [ -e "$health/running" ]; then reason=unclean; fi
    touch "$health/running"
    sync -f "$persist"
    printf '%s\n' "$reason"
    ;;
  check)
    reason=$3
    [ -n "$reason" ] || exit 0
    echo 'TSUGUMI-STAGE checkingStore'
    echo 'Checking the Nix store after an unclean stop; projects will wait.'
    # Keep the full diagnostics (including paths repaired or unavailable) and
    # the actual exit status. A pipeline ending in tail hid failures before.
    touch "$health/running"
    sync -f "$persist"
    status=0
    NIX_REMOTE=local NO_COLOR=1 nix-store --verify --check-contents --repair 2>&1 | tee "$health/check.log" || status=$?
    if [ "$status" -ne 0 ]; then
      echo "Store check/repair failed (exit $status); projects were not started. See $health/check.log."
      sync -f "$persist"
      exit "$status"
    fi
    echo 'Store content check and repair completed. Repairs, if any, are listed above.'
    sync -f "$persist"
    ;;
  poweroff)
    # The store is written only through nix-daemon (builds run under it).
    # The overlay on /nix cannot be remounted read-only (the kernel refuses to
    # reconfigure it), so end every daemon and wait until none is left: after
    # that nothing can change registered store data. A daemon that survives
    # keeps the dirty marker, and the next boot checks the store.
    "$3" --stop-nix
    sync
    # Flush the store's backing filesystem, then durably clear the marker.
    bash "$0" clean "$persist"
    ;;
  clean)
    # All writers must already be stopped. Flush data BEFORE clearing the
    # marker, then persist the unlink before poweroff.
    sync
    sync -f "$persist"
    rm -f "$health/running"
    sync -f "$persist"
    ;;
  *) exit 2 ;;
esac
