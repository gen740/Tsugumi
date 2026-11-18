# Export mode of the pack guest (sourced by /init, network on).
#
#   /pack/project   the Project as tsugumi-pack staged it (read only)
#   /pack/out       payload.erofs, cards.jsonl and export.json for the app
#   /persist        the exporter's store disk: builds stay for the next export
#
# tsugumid --prepare-cards builds every Code Card the way a run would, and
# the payload is everything a run then needs offline, minus what the base
# image already has: each Card's runtime closure, its derivations (the run's
# evaluation finds them instead of writing them again) and the nixpkgs
# sources the Cards are evaluated with.
set -euo pipefail
stage() { echo "PACK-STAGE $*"; }
out=/pack/out
work=/persist/export
rm -rf "$work"
mkdir -p "$work" "$out"
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
# The nixpkgs each project file pins (the Card in Card projects under
# externals/ may pin others); fetched by the build, so evaluation finds them.
pins=()
while IFS= read -r spec; do
  case "$spec" in
  tsugumi:default | "") url="https://github.com/NixOS/nixpkgs/archive/$CARD_NIXPKGS_REV.tar.gz" hash="$CARD_NIXPKGS_NARHASH" ;;
  *'#'*) url="${spec%%#*}" hash="${spec#*#}" ;;
  *) echo "unsupported pkgsPath $spec"; exit 1 ;;
  esac
  pins+=("$(nix eval --raw --impure --expr "builtins.fetchTarball { url = \"$url\"; sha256 = \"$hash\"; }")")
done < <(find "$work/project" -name tsugumi.json -not -path '*/.tsugumi/*' -exec jq -r '.pkgsPath // ""' {} \; | sort -u)
mapfile -t roots < <(jq -r 'select(.manifest) | .manifest, .derivation' "$out/cards.jsonl")
nix-store -qR "${roots[@]}" "${pins[@]}" | sort -u > "$work/all-paths"
sort -u /store-paths > "$work/base-paths"
comm -23 "$work/all-paths" "$work/base-paths" > "$work/payload-paths"
echo "payload: $(wc -l < "$work/payload-paths") store paths beyond the base image"

stage image
stage=$work/stage
mkdir -p "$stage/nix/store"
while read -r p; do cp -a "$p" "$stage/nix/store/"; done < "$work/payload-paths"
nix-store --dump-db $(cat "$work/payload-paths") > "$stage/registration"
cp "$work/payload-paths" "$stage/store-paths"
# The Project as it was exported, without the state the build created.
mkdir -p "$stage/project"
(cd /pack/project && tar -cf - --exclude=./.tsugumi .) | tar -xf - -C "$stage/project"
cp "$out/cards.jsonl" "$stage/cards.jsonl"
rm -f "$out/payload.erofs"
mkfs.erofs -zlz4hc -T0 --all-root "$out/payload.erofs" "$stage" >/dev/null
jq -n --argjson cards "$(jq -s . "$out/cards.jsonl")" \
  --arg paths "$(wc -l < "$work/payload-paths")" \
  --arg bytes "$(stat -c %s "$out/payload.erofs")" \
  '{cards: $cards, payload_paths: ($paths | tonumber), payload_bytes: ($bytes | tonumber)}' > "$out/export.json"
rm -rf "$work"
echo "payload.erofs: $(($(stat -c %s "$out/payload.erofs") >> 20)) MiB"
