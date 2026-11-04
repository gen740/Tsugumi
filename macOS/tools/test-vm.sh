#!/usr/bin/env bash
# The same entry point locally and on the dedicated CI Mac.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo"
if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
  echo 'The shared VM tests require an Apple Silicon Mac.' >&2
  exit 2
fi
for tool in nix swift codesign; do
  command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 2; }
done
if [ "${1:-}" = --check ]; then
  echo 'Shared VM test prerequisites found.'
  exit 0
fi
if [ "$#" -ne 0 ]; then
  echo 'usage: macOS/tools/test-vm.sh [--check]' >&2
  exit 2
fi
reports="$repo/build/vm-ci"
mkdir -p "$reports"
# Clear only this entry point's old reports, never VM disks or project data.
rm -f "$reports/pytest.xml" "$reports/pytest.log" "$reports/build.log" \
  "$reports/vm-console.log" "$reports/launcher.log" "$reports/vmctl-console.log"
export TSUGUMI_TEST_VM_SUPPORT="${TSUGUMI_TEST_VM_SUPPORT:-$repo/build/test-vm}"
collect_logs() {
  local entry
  for entry in 'vm/console.log:vm-console.log' 'vm/launcher.log:launcher.log' 'vmctl-console.log:vmctl-console.log'; do
    if [ -f "$TSUGUMI_TEST_VM_SUPPORT/${entry%%:*}" ]; then
      cp "$TSUGUMI_TEST_VM_SUPPORT/${entry%%:*}" "$reports/${entry#*:}"
    fi
  done
}
trap collect_logs EXIT
{
  macOS/Container/build-image.sh --vm
  macOS/tools/build-vmctl.sh
  nix develop -c bash -c 'cmake -S . -B build -G Ninja && ninja -C build tsugumid'
} 2>&1 | tee "$reports/build.log"
export TSUGUMI_VMCTL="$repo/macOS/.build/debug/tsugumi-vmctl"
export TSUGUMI_TEST_CONTAINER=1
export TMPDIR=/tmp
nix develop -c python3 -m pytest -q \
  tests/python/test_shared_vm.py tests/python/test_workers.py tests/python/test_cards_container.py \
  --junitxml="$reports/pytest.xml" 2>&1 | tee "$reports/pytest.log"
# A disabled marker or accidental test-selection change must not pass this job.
nix develop -c python3 - "$reports/pytest.xml" <<'PY'
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
cases = list(root.iter('testcase'))
if not cases or any(case.find('skipped') is not None for case in cases):
    raise SystemExit('Shared VM CI must execute every selected test (no skips).')
PY
