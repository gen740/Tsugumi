# Tsugumi in the shared VM: built by Nix inside the VM from the source the
# app shares (vm-boot.nix's /init), so nothing of Tsugumi is downloaded to
# the Mac. Rebuilt when the source changes; its dependencies come from
# cache.nixos.org and stay in the store disk.
#
#   $out/supervisor   tsugumi-vmd with the tools projects see, run under tini
#   $out/tini         PID 1 of the VM's service and of each project
#
# Tools a project's terminal and Card shell steps see, and the source
# formatters (ProjectFileFormat). tsugumid's own run time tools (bubblewrap,
# sqlite, jq) come with its closure.
{ lib, runCommand, writeShellScript, tsugumid, tini, bashInteractive, cacert, coreutils, findutils, gnugrep
, gnused, gawk, gnutar, gzip, nix, astyle, ruff, nixfmt, shfmt }:
let
  formatters = [ astyle ruff nixfmt shfmt ];
  projectTools = [ bashInteractive coreutils findutils gnugrep gnused gawk gnutar gzip nix ] ++ formatters;
  supervisor = writeShellScript "tsugumi-supervisor" ''
    exec ${tsugumid}/bin/tsugumi-vmd \
      --tsugumid ${tsugumid}/bin/tsugumid --tini ${tini}/bin/tini --bash ${bashInteractive}/bin/bash \
      --path ${lib.makeBinPath projectTools} --cert-file ${cacert}/etc/ssl/certs/ca-bundle.crt
  '';
in
runCommand "tsugumi-vm-system-${tsugumid.version}" { } ''
  mkdir -p $out
  ln -s ${supervisor} $out/supervisor
  ln -s ${tini}/bin/tini $out/tini
''
