# Run as root with the boot image's tools on PATH. No projects may be running.
set -euo pipefail
action=$1
persist=$2
health="$persist/store-health"
version=1
mkdir -p "$health"
chmod 700 "$health"

case "$action" in
  begin)
    # Decide before replacing the marker. A missing version also checks every
    # existing disk once when this protection is first installed.
    reason=""
    if [ -e "$health/running" ]; then reason=unclean
    elif [ "$(cat "$health/checked-version" 2>/dev/null || true)" != "$version" ]; then reason=upgrade
    elif [ "$(cat "$health/boot-closure" 2>/dev/null || true)" != "$3" ]; then reason=image
    fi
    touch "$health/running"
    sync -f "$persist"
    printf '%s\n' "$reason"
    ;;
  check)
    boot=$3
    reason=$4
    [ -n "$reason" ] || exit 0
    if [ "$reason" = unclean ]; then
      echo 'TSUGUMI-STAGE checkingStore'
      echo 'Checking the Nix store after an unclean stop; projects will wait.'
    else
      echo 'TSUGUMI-STAGE checkingStoreUpgrade'
      echo 'Checking the Nix store contents before starting projects.'
    fi
    # Keep the full diagnostics (including paths repaired or unavailable) and
    # the actual exit status. A pipeline ending in tail hid failures before.
    touch "$health/running"
    rm -f "$health/checked-version"
    sync -f "$persist"
    status=0
    NIX_REMOTE=local NO_COLOR=1 nix-store --verify --check-contents --repair 2>&1 | tee "$health/check.log" || status=$?
    if [ "$status" -ne 0 ]; then
      echo "Store check/repair failed (exit $status); projects were not started. See $health/check.log."
      sync -f "$persist"
      exit "$status"
    fi
    echo 'Store content check and repair completed. Repairs, if any, are listed above.'
    printf '%s\n' "$version" > "$health/checked-version"
    printf '%s\n' "$boot" > "$health/boot-closure"
    sync -f "$persist"
    ;;
  poweroff)
    # Stop daemon connections/builds before freezing the shared overlay. If a
    # writer survives, read-only remount failure keeps the dirty marker.
    for p in /proc/[0-9]*; do
      [ "$(cat "$p/comm" 2>/dev/null)" != nix-daemon ] || kill -TERM "${p##*/}" 2>/dev/null || true
    done
    sync
    mount -o remount,ro /history-disk || exit 1
    mount -o remount,ro /nix || exit 1
    # The overlay is now read-only: no registered store data can change.
    # Do not remount its backing filesystem read-only: the mounted overlay
    # holds /persist/nix/upper and work open for writing even when it is ro.
    # Flush the backing filesystem instead, then durably clear the marker.
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
