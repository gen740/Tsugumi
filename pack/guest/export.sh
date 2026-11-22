# Export mode of the pack guest (run by its /init, network on).
#
#   /pack/project   the Project as tsugumi-pack staged it (read only)
#   /pack/out       payload.erofs, cards.jsonl and export.json for the app
#   /work           the work folder, as in a run (the exporter's store disk)
#
# tsugumid --prepare-cards evaluates and builds every Code Card the way a run
# would, and records each one's evaluation key and build. The payload is the
# Cards' runtime closures, minus what the app's runner image already holds:
# a run uses these builds and evaluates nothing, so neither nixpkgs nor any
# derivation goes into the app.
set -euo pipefail
stage() { echo "PACK-STAGE $*"; }
out=/pack/out
work=/work
find "$work" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
mkdir -p "$out"
cp -a /pack/project "$work/project"
chmod -R u+w "$work/project"

stage build
started=$(date +%s)
if ! "$TSUGUMID" --project "$work/project" --prepare-cards "$out/cards.jsonl" \
  --cpus "$(nproc)" --scratch-dir "$work/scratch" --run-scratch-dir "$work/run-scratch" \
  --gcroots-dir /nix/var/nix/gcroots/pack 2>&1 | sed -u 's/^/tsugumid: /'; then
  echo "some Cards did not build:"
  jq -r 'select(.error) | "  \(.card): \(.error)"' "$out/cards.jsonl"
  exit 1
fi
echo "built $(jq -s length "$out/cards.jsonl") Cards in $(($(date +%s) - started)) s"

stage closure
mapfile -t plans < <(jq -r '.plan' "$out/cards.jsonl")
if [ ${#plans[@]} -gt 0 ]; then nix-store -qR "${plans[@]}"; fi | sort -u > "$work/all-paths"
sort -u "$RUN_STORE_PATHS" > "$work/base-paths"
comm -23 "$work/all-paths" "$work/base-paths" > "$work/payload-paths"
echo "payload: $(wc -l < "$work/payload-paths") store paths beyond the runner image"

stage image
stage=$work/stage
mkdir -p "$stage/nix/store"
while read -r p; do cp -a "$p" "$stage/nix/store/"; done < "$work/payload-paths"
# The Project as it was exported, without the state the build created.
mkdir -p "$stage/project"
(cd /pack/project && tar -cf - --exclude=./.tsugumi .) | tar -xf - -C "$stage/project"
cp "$out/cards.jsonl" "$stage/cards.jsonl"
rm -f "$out/payload.erofs"
mkfs.erofs -zlz4hc -T0 --all-root "$out/payload.erofs" "$stage" >/dev/null
jq -n --argjson cards "$(jq -s 'map(del(.closure, .key))' "$out/cards.jsonl")" \
  --arg paths "$(wc -l < "$work/payload-paths")" \
  --arg bytes "$(stat -c %s "$out/payload.erofs")" \
  '{cards: $cards, payload_paths: ($paths | tonumber), payload_bytes: ($bytes | tonumber)}' > "$out/export.json"
find "$work" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
echo "payload.erofs: $(($(stat -c %s "$out/payload.erofs") >> 20)) MiB"
