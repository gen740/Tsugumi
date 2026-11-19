# `nix run .#guest-image`: puts the release-built guest image of the working
# tree in dist/tsugumi-vm. The image is never built here unless asked: the
# guest definition (snapshot of its inputs, see vm/guest/tools/vm-snapshot.py)
# names one identity, and the image is
#   - dist/tsugumi-vm when it verifies for that identity, or
#   - the successful Actions artifact of that identity (`gh auth login`).
# Otherwise the command stops and says so.
#
#   nix run .#guest-image                  the image, from an artifact
#   nix run .#guest-image -- --release     dist/tsugumi-vm must be the artifact of HEAD (release packaging)
#   nix run .#guest-image -- --build       build the image here
#   nix run .#guest-image -- --kernel      build the guest kernel into dist/tsugumi-kernel
#   nix run .#guest-image -- --check       build the guest's tsugumid
#
# Building here takes much memory: on Linux with Nix (or Nix builders in
# TSUGUMI_VM_NIX_BUILDERS), on a Mac in an Apple container
# (TSUGUMI_BUILDER_CPUS, TSUGUMI_BUILDER_MEMORY, TSUGUMI_BUILDER_VOLUME).
{
  writeShellApplication,
  python3,
  git,
  gh,
}:
writeShellApplication {
  name = "tsugumi-guest-image";
  runtimeInputs = [
    python3
    git
    gh
  ];
  text = ''
    repo="$(git rev-parse --show-toplevel)"
    if [ ! -f "$repo/vm/guest/versions.env" ]; then
      echo "guest-image: run this inside the Tsugumi repository" >&2
      exit 1
    fi
    # shellcheck source=/dev/null
    source "$repo/vm/guest/versions.env"
    mode=artifact
    attr=vm
    for arg in "$@"; do
      case "$arg" in
      --release) mode=release ;;
      --build) mode=build ;;
      --kernel) mode=build attr=kernel ;;
      --check) mode=build attr=tsugumid ;;
      *)
        echo "usage: nix run .#guest-image -- [--release | --build | --kernel | --check]" >&2
        exit 2
        ;;
      esac
    done
    stage="$(mktemp -d "''${TMPDIR:-/tmp}/tsugumi-src.XXXXXX")"
    out="$(mktemp -d "''${TMPDIR:-/tmp}/tsugumi-image.XXXXXX")"
    builder=""
    # The snapshot holds copies of read-only store files: make them removable.
    trap '[ -z "$builder" ] || container stop "$builder" >/dev/null 2>&1 || true; chmod -R u+w "$stage" "$out" 2>/dev/null; rm -rf "$stage" "$out"' EXIT
    # Current tracked and untracked inputs of the guest, including local edits.
    python3 "$repo/vm/guest/tools/vm-snapshot.py" "$repo" "$stage"
    revision="$(git -C "$repo" rev-parse HEAD)"
    if [ -n "$(git -C "$repo" status --porcelain)" ]; then revision="$revision-dirty"; fi
    dist_vm="$repo/dist/tsugumi-vm"
    verify=(python3 "$stage/vm/guest/tools/vm-artifact.py" verify)

    if [ "$mode" != build ]; then
      identity="$(nix eval --raw "path:$stage#packages.aarch64-linux.vm-boot.outPath")"
      identity="''${identity##*/}"
      if [ "$mode" = release ]; then
        "''${verify[@]}" "$dist_vm" "$identity" --revision "$revision"
        exit 0
      fi
      if "''${verify[@]}" "$dist_vm" "$identity" 2>/dev/null; then
        echo "guest image: reusing $identity"
        exit 0
      fi
      if python3 "$stage/vm/guest/tools/vm-download.py" "$identity" "$out" "''${GITHUB_REPOSITORY:-gen740/Tsugumi}" &&
        "''${verify[@]}" "$out" "$identity"; then
        mkdir -p "$repo/dist"
        rm -rf "$dist_vm"
        mv "$out" "$dist_vm"
        echo "guest image: $identity from Actions"
        exit 0
      fi
      echo "guest image: no verified dist/tsugumi-vm and no Actions artifact for $identity." >&2
      echo "Push the change and let CI build it (gh auth login to download it), or build it here with --build." >&2
      exit 1
    fi

    if [ "$(uname -s)" = Linux ] || [ -n "''${TSUGUMI_VM_NIX_BUILDERS:-}" ]; then
      build_args=()
      if [ -n "''${TSUGUMI_VM_NIX_BUILDERS:-}" ]; then
        build_args=(--builders "$TSUGUMI_VM_NIX_BUILDERS")
        export NIX_CONFIG="''${NIX_CONFIG:-}
    builders = $TSUGUMI_VM_NIX_BUILDERS"
      fi
      image="$(nix build --no-link --print-out-paths -L "''${build_args[@]}" "path:$stage#packages.aarch64-linux.$attr")"
      if [ "$attr" = vm ]; then
        python3 "$stage/vm/guest/tools/vm-artifact.py" build "$image" "$out" --revision "$revision"
      elif [ "$attr" = kernel ]; then cp "$image/vmlinux" "$out/tsugumi-kernel"; fi
    else
      kernel_args=()
      [ ! -f "$repo/dist/tsugumi-kernel" ] || kernel_args=(--kernel "$repo/dist/tsugumi-kernel")
      volume="''${TSUGUMI_BUILDER_VOLUME:-tsugumi-builder-nix}"
      container volume inspect "$volume" >/dev/null 2>&1 || container volume create -s 40G "$volume" >/dev/null
      builder="tsugumi-image-builder-$(date +%s)"
      # Keep the builder image's bootstrap /nix visible; the build store is
      # under /cache (vm/guest/build-in-container.sh).
      # shellcheck disable=SC2016
      container run -d --rm --name "$builder" "''${kernel_args[@]}" -c "''${TSUGUMI_BUILDER_CPUS:-8}" -m "''${TSUGUMI_BUILDER_MEMORY:-16G}" \
        -v "$volume:/cache" -v "$stage:/src:ro" -v "$out:/out" \
        -e "GUEST_ATTRIBUTE=$attr" -e "GUEST_REVISION=$revision" \
        "$TSUGUMI_BUILDER_IMAGE" sh -uc '
          trap "[ -e /out/done ] || touch /out/failed" EXIT
          set -e
          exec > /out/log 2>&1
          # The builder image enables no experimental features.
          export NIX_CONFIG="experimental-features = nix-command flakes"
          nix shell --inputs-from /src nixpkgs#nix nixpkgs#python3 -c sh /src/vm/guest/build-in-container.sh
          sync -f /cache
          touch /out/done
        ' >/dev/null
      echo "building $attr in $builder (progress: $out/log)"
      while [ ! -e "$out/done" ] && [ ! -e "$out/failed" ]; do
        if ! container inspect "$builder" >/dev/null 2>&1; then
          echo "guest image: builder container $builder exited unexpectedly" >&2
          touch "$out/failed"
          break
        fi
        sleep 2
      done
      builder=""
      [ ! -f "$out/log" ] || tail -10 "$out/log"
      [ ! -e "$out/failed" ] || exit 1
      rm -f "$out/done" "$out/log"
    fi
    mkdir -p "$repo/dist"
    if [ "$attr" = vm ]; then
      rm -rf "$dist_vm"
      mv "$out" "$dist_vm"
    elif [ "$attr" = kernel ]; then
      cp "$out/tsugumi-kernel" "$repo/dist/tsugumi-kernel"
    fi
  '';
}
