#!/bin/sh
# Generates the TypeScript bindings (object API) and the RPC BFBS from
# protocol/*.fbs with the pinned flatc (run inside `nix develop`).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/../.." && pwd)
out="$here/shared/gen"
rm -rf "$out"
mkdir -p "$out"
# First: its includes rewrite namespace index files (tsugumi/data) with only
# the types it reaches, and the full schemas below write them again.
flatc --ts --gen-all --gen-object-api --ts-no-import-ext -I "$root/protocol" -o "$out" "$root/protocol/remote_worker.fbs"
for schema in tsugumi agent host; do
  flatc --ts --gen-all --gen-object-api --ts-no-import-ext -I "$root/protocol" -o "$out" "$root/protocol/$schema.fbs"
  flatc -b --schema --bfbs-comments --bfbs-builtins -I "$root/protocol" -o "$out" "$root/protocol/$schema.fbs"
done
