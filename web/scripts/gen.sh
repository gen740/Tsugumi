#!/bin/sh
# Generates the TypeScript bindings (object API) and the RPC BFBS from
# protocol/*.fbs with the pinned flatc (run inside `nix develop`).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/.." && pwd)
out="$here/src/gen"
rm -rf "$out"
mkdir -p "$out"
flatc --ts --gen-all --gen-object-api --ts-no-import-ext -I "$root/protocol" -o "$out" "$root/protocol/tsugumi.fbs"
flatc -b --schema --bfbs-comments --bfbs-builtins -I "$root/protocol" -o "$out" "$root/protocol/tsugumi.fbs"
flatc --ts --gen-all --gen-object-api --ts-no-import-ext -I "$root/protocol" -o "$out" "$root/protocol/agent.fbs"
flatc -b --schema --bfbs-comments --bfbs-builtins -I "$root/protocol" -o "$out" "$root/protocol/agent.fbs"

flatc --ts --gen-all --gen-object-api --ts-no-import-ext -I "$root/protocol" -o "$out" "$root/protocol/remote_worker.fbs"
