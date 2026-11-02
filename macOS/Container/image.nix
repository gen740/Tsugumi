# Minimal Linux image for one Tsugumi project environment (Apple container).
#
# Contents and why each is needed:
#   tsugumid       core daemon, worker and Card SDK
#   nix            builds Card packages from their default.nix
#   cacert         TLS for cache.nixos.org and the pinned nixpkgs archive
#   bash/coreutils/util-linux  the entrypoint (overlay mount of the store)
#   tini           PID 1: reaps orphaned processes (sandbox helpers) and forwards signals
#   astyle, ruff, nixfmt, shfmt  source formatting (ProjectFileFormat) of
#                  C++, Python, Nix and shell (astyle instead of clang-format,
#                  which would add the whole LLVM, about 500 MB)
# Card compilers, Python and libraries are NOT in the image: they are fetched
# as pinned Nix dependencies on first use into the persistent store volume.
{ lib, dockerTools, writeShellScript, writeTextDir, closureInfo, buildEnv, tsugumid, nix, cacert
, bashInteractive, coreutils, util-linux, tini, astyle, ruff, nixfmt-rfc-style, shfmt }:
let
  nixConf = writeTextDir "etc/nix/nix.conf" ''
    build-users-group =
    sandbox = true
    experimental-features = nix-command
    substituters = https://cache.nixos.org
    trusted-public-keys = cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY=
    max-jobs = auto
    allow-import-from-derivation = false
    keep-outputs = false
  '';
  etc = buildEnv {
    name = "tsugumi-etc";
    paths = [
      nixConf
      (writeTextDir "etc/passwd" "root:x:0:0:root:/root:/bin/sh\nnobody:x:65534:65534:nobody:/var/empty:/bin/false\n")
      (writeTextDir "etc/group" "root:x:0:\nnobody:x:65534:\n")
    ];
  };
  formatters = [ astyle ruff nixfmt-rfc-style shfmt ];
  runtime = [ tsugumid nix cacert bashInteractive coreutils util-linux tini etc ] ++ formatters;
  registration = closureInfo { rootPaths = runtime; };
  entrypoint = writeShellScript "tsugumi-entrypoint" ''
    set -euo pipefail
    export PATH=${lib.makeBinPath ([ coreutils util-linux nix bashInteractive ] ++ formatters)}
    export NIX_SSL_CERT_FILE=${cacert}/etc/ssl/certs/ca-bundle.crt
    export SSL_CERT_FILE=$NIX_SSL_CERT_FILE
    export HOME=/root TMPDIR=/tmp USER=root
    mkdir -p /tmp /root /run/tsugumi && chmod 1777 /tmp && chmod 700 /run/tsugumi
    # The image store is the lower layer; Card builds land in the persistent volume.
    if [ -d /persist ] && ! mountpoint -q /nix; then
      mkdir -p /persist/nix/upper /persist/nix/work
      # Paths of this image deleted in the upper layer (whiteouts, e.g. by an
      # older garbage collection) are restored before the overlay is mounted.
      if [ -d /persist/nix/upper/store ]; then
        for f in /persist/nix/upper/store/*; do
          if [ -c "$f" ] && [ -e "/nix/store/''${f##*/}" ]; then rm -f "$f"; fi
        done
      fi
      # index=off: the upper layer stays valid when a new image changes the lower layer.
      mount -t overlay overlay -o lowerdir=/nix,upperdir=/persist/nix/upper,workdir=/persist/nix/work,index=off,redirect_dir=off,metacopy=off,xino=off /nix
    fi
    nix-store --load-db < ${registration}/registration
    # The environment's own closure is a GC root: CacheClear's garbage
    # collection removes Card builds only, never the runtime.
    mkdir -p /nix/var/nix/gcroots/tsugumi
    for p in ${lib.concatStringsSep " " (map toString (runtime ++ [ registration ]))}; do
      ln -sfn "$p" "/nix/var/nix/gcroots/tsugumi/''${p##*/}"
    done
    # tini stays PID 1 so exited sandbox helpers never accumulate as zombies.
    exec ${tini}/bin/tini -g -- ${tsugumid}/bin/tsugumid --project /project --listen 0.0.0.0:7700 \
      --unix /run/tsugumi/tsugumid.sock --control-dir /control \
      --cpus "''${TSUGUMI_CPUS:-2}" --own-nix-store --allow-missing-origin "$@"
  '';
in
dockerTools.buildLayeredImage {
  name = "tsugumi-env";
  tag = tsugumid.version;
  contents = runtime;
  extraCommands = "mkdir -p tmp root project control persist var/empty";
  config = {
    Entrypoint = [ "${entrypoint}" ];
    Env = [ "PATH=/bin" ];
    WorkingDir = "/";
  };
  maxLayers = 40;
}
