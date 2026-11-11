# Tsugumi in the shared VM: built by Nix inside the VM from the source the
# app shares (vm-boot.nix's /init), so nothing of Tsugumi is downloaded to
# the Mac. Rebuilt when the source changes; its dependencies come from
# cache.nixos.org and stay in the store disk.
#
#   $out/supervisor   tsugumi-vmd with the tools projects see, run under tini
#   $out/tini         PID 1 of the VM's service and of each project
#
# x86_64-linux binaries (Cards with "system": "x86_64-linux", and their Nix
# builds) run with what the app names on the kernel command line
# (SharedVM.swift): tsugumi.x86=rosetta, Rosetta's share from the Mac;
# tsugumi.x86=qemu, only when the user asked for it (TSUGUMI_VM_X86=qemu),
# qemu-user fetched from cache.nixos.org then; tsugumi.x86=none, not at all.
# The supervisor registers it with binfmt_misc and adds x86_64-linux to Nix's
# extra-platforms before any project starts.
#
# Tools a project's terminal and Card shell steps see, and the source
# formatters (ProjectFileFormat). tsugumid's own run time tools (bubblewrap,
# sqlite, jq) come with its closure.
{ lib, runCommand, writeShellScript, tsugumid, tini, bashInteractive, cacert, coreutils, findutils, gnugrep
, gnused, gawk, gnutar, gzip, nix, util-linux, qemu-user, astyle, ruff, nixfmt, shfmt }:
let
  formatters = [ astyle ruff nixfmt shfmt ];
  projectTools = [ bashInteractive coreutils findutils gnugrep gnused gawk gnutar gzip nix ] ++ formatters;
  # Not part of this closure: fetched only when qemu is asked for.
  qemu = builtins.unsafeDiscardStringContext qemu-user.outPath;
  x86 = writeShellScript "tsugumi-x86_64" ''
    export PATH=${lib.makeBinPath [ coreutils gnused util-linux nix ]}
    mode=$(sed -n 's/.*tsugumi\.x86=\([a-z]*\).*/\1/p' /proc/cmdline)
    sandbox=""
    case "$mode" in
      rosetta)
        mkdir -p /run/rosetta
        mount -t virtiofs -o ro rosetta /run/rosetta || { echo "x86_64-linux: Rosetta's share is missing"; exit 0; }
        interpreter=/run/rosetta/rosetta
        # Rosetta reads its own folder while it runs: Nix's build sandbox sees it.
        sandbox=/run/rosetta ;;
      qemu)
        NIX_REMOTE=daemon nix-store --realise ${qemu} >/dev/null || { echo "x86_64-linux: cannot fetch qemu-user"; exit 0; }
        interpreter=${qemu}/bin/qemu-x86_64
        # Dynamically linked: Nix's build sandbox sees its libraries.
        sandbox=$(NIX_REMOTE=daemon nix-store -qR ${qemu} | tr '\n' ' ') ;;
      *) echo "x86_64-linux: no emulator (install Rosetta, or set TSUGUMI_VM_X86=qemu)"; exit 0 ;;
    esac
    # F: the kernel opens the interpreter now, so sandboxes that do not see
    # its path run x86_64 binaries all the same.
    mountpoint -q /proc/sys/fs/binfmt_misc || mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
    printf '%s' ":x86_64-linux:M::\\x7fELF\\x02\\x01\\x01\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x00\\x02\\x00\\x3e\\x00:\\xff\\xff\\xff\\xff\\xff\\xfe\\xfe\\x00\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xff\\xfe\\xff\\xff\\xff:$interpreter:F" \
      > /proc/sys/fs/binfmt_misc/register || { echo "x86_64-linux: cannot register $interpreter"; exit 0; }
    # nix.conf (a link into the boot disk's store) becomes a file of /etc's
    # writable copy, and nix-daemon starts again to read it.
    conf=$(cat /etc/nix/nix.conf)
    if [ -L /etc/nix ]; then target=$(readlink -f /etc/nix); rm /etc/nix; mkdir /etc/nix; cp -L "$target"/* /etc/nix/; fi
    rm -f /etc/nix/nix.conf
    printf '%s\nextra-platforms = x86_64-linux\nextra-sandbox-paths = %s\n' "$conf" "$sandbox" > /etc/nix/nix.conf
    for p in /proc/[0-9]*; do [ "$(cat "$p/comm" 2>/dev/null)" = nix-daemon ] && kill "''${p#/proc/}"; done
    for i in $(seq 50); do [ -S /nix/var/nix/daemon-socket/socket ] || break; sleep 0.1; done
    rm -f /nix/var/nix/daemon-socket/socket
    nix-daemon --daemon >>/persist/nix-daemon.log 2>&1 &
    for i in $(seq 100); do [ -S /nix/var/nix/daemon-socket/socket ] && break; sleep 0.1; done
    echo "x86_64-linux: $mode ($interpreter)"
  '';
  supervisor = writeShellScript "tsugumi-supervisor" ''
    ${x86}
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
