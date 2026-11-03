# Tsugumi on top of the shared VM's base (vm-base.nix): the layer disk.
#
#   $out/tsugumi.erofs  read-only, mounted by the base's /init as /layer:
#     nix/store/          the store paths of this closure that the base lacks
#     registration, store-paths   this whole closure (closureInfo)
#     supervisor          tsugumi-vmd with the base's tools, run under tini
#
# Only this is built for each change of the core; the base is downloaded.
{ lib, runCommand, writeShellScript, closureInfo, erofs-utils, tsugumid, base, tini, bashInteractive, cacert }:
let
  supervisor = writeShellScript "tsugumi-supervisor" ''
    exec ${tsugumid}/bin/tsugumi-vmd \
      --tsugumid ${tsugumid}/bin/tsugumid --tini ${tini}/bin/tini --bash ${bashInteractive}/bin/bash \
      --path ${lib.makeBinPath base.projectTools} --cert-file ${cacert}/etc/ssl/certs/ca-bundle.crt
  '';
  closure = closureInfo { rootPaths = [ supervisor ]; };
  inBase = closureInfo { rootPaths = base.runtime; };
in
runCommand "tsugumi-vm-layer-${tsugumid.version}" { nativeBuildInputs = [ erofs-utils ]; } ''
  root=$TMPDIR/root
  mkdir -p $root/nix/store
  grep -vxF -f ${inBase}/store-paths ${closure}/store-paths > $TMPDIR/own || true
  for p in $(cat $TMPDIR/own); do cp -a "$p" $root/nix/store/; done
  echo "layer: $(wc -l < $TMPDIR/own) of $(wc -l < ${closure}/store-paths) store paths"
  cp ${closure}/registration $root/registration
  cp ${closure}/store-paths $root/store-paths
  ln -s ${supervisor} $root/supervisor
  mkdir -p $out
  mkfs.erofs -zlz4hc -T0 --all-root $out/tsugumi.erofs $root
''
