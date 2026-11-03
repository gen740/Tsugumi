# The shared Tsugumi VM (one per app, run with Virtualization.framework).
#
#   $out/vmlinux        guest kernel (kernel.nix)
#   $out/rootfs.erofs   read-only root file system: /init, this closure, /etc
#
# Disks and shares the app gives the VM:
#   /dev/vda    rootfs.erofs (read-only)
#   /dev/vdb    the Nix store disk (sparse file in the app's support folder),
#               formatted on first boot and mounted as /persist; /nix is the
#               image's store (lower) + /persist/nix (upper) as an overlay
#   virtiofs "tsugumi-<n>" (16 slots): one per open project, with its
#               `project` and `control` folders, at /mnt/slots/<n> (tsugumi-vmd)
#   hvc0        console: everything here and in tsugumi-vmd is logged to it,
#               with "TSUGUMI-STAGE <name>" lines for the app's progress
#   vsock       1024 control, 1025 RPC (tsugumi-vmd)
#
# Contents and why each is needed:
#   tsugumid, tsugumi-vmd  core daemon (one per project) and its supervisor
#   nix            the one nix-daemon of the shared store, and the clients
#   cacert         TLS for cache.nixos.org and the pinned nixpkgs archive
#   bash/coreutils/util-linux/e2fsprogs  /init (store disk, overlay)
#   busybox        udhcpc and ip for the NAT network
#   tini           PID 1 of the VM and of each project
#   astyle, ruff, nixfmt, shfmt  source formatting (ProjectFileFormat)
{ lib, runCommand, writeShellScript, writeTextDir, closureInfo, buildEnv, erofs-utils, tsugumid, kernel, nix
, cacert, bashInteractive, coreutils, util-linux, e2fsprogs, busybox, tini, findutils, gnugrep, gnused, gawk
, gnutar, gzip, astyle, ruff, nixfmt-rfc-style, shfmt }:
let
  nixbld = map toString (lib.range 1 16);
  nixConf = writeTextDir "etc/nix/nix.conf" ''
    build-users-group = nixbld
    sandbox = true
    experimental-features = nix-command
    substituters = https://cache.nixos.org
    trusted-public-keys = cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY=
    max-jobs = auto
    allow-import-from-derivation = false
    # Build-time dependencies (compilers, the nixpkgs source) of every rooted
    # Card build stay through a store cleanup, so editing a Card rebuilds
    # without downloading them again.
    keep-outputs = true
    keep-derivations = true
    allowed-users = *
    trusted-users = root
  '';
  etc = buildEnv {
    name = "tsugumi-vm-etc";
    paths = [
      nixConf
      (writeTextDir "etc/passwd" (''
        root:x:0:0:root:/root:/bin/sh
        nobody:x:65534:65534:nobody:/var/empty:/bin/false
      '' + lib.concatMapStrings (i: "nixbld${i}:x:${toString (30000 + lib.toInt i)}:30000:nixbld:/var/empty:/bin/false\n") nixbld))
      (writeTextDir "etc/group" ''
        root:x:0:
        nixbld:x:30000:${lib.concatMapStringsSep "," (i: "nixbld${i}") nixbld}
        nobody:x:65534:
      '')
      (writeTextDir "etc/hosts" "127.0.0.1 localhost\n")
      (writeTextDir "etc/nsswitch.conf" "hosts: files dns\n")
    ];
  };
  formatters = [ astyle ruff nixfmt-rfc-style shfmt ];
  # Tools a project's terminal and Card shell steps see.
  projectTools = [ bashInteractive coreutils findutils gnugrep gnused gawk gnutar gzip nix ] ++ formatters;
  udhcpcScript = writeShellScript "tsugumi-udhcpc" ''
    PATH=${busybox}/bin
    case "$1" in
      deconfig) ip addr flush dev "$interface"; ip link set "$interface" up ;;
      bound|renew)
        ip addr flush dev "$interface"
        ip addr add "$ip/$mask" dev "$interface"
        for r in $router; do ip route add default via "$r" dev "$interface"; break; done
        : > /run/resolv.conf
        for d in $dns; do echo "nameserver $d" >> /run/resolv.conf; done
        echo "network: $ip/$mask via $router, dns $dns"
        ;;
    esac
  '';
  init = writeShellScript "tsugumi-vm-init" ''
    export PATH=${lib.makeBinPath [ coreutils util-linux e2fsprogs nix bashInteractive gawk ]}
    export NIX_SSL_CERT_FILE=${cacert}/etc/ssl/certs/ca-bundle.crt
    export SSL_CERT_FILE=$NIX_SSL_CERT_FILE HOME=/root TMPDIR=/tmp USER=root
    exec </dev/console >/dev/console 2>&1
    stage() { echo "TSUGUMI-STAGE $*"; }
    fail() { echo "TSUGUMI-FAILED $*"; sleep 1; ${busybox}/bin/poweroff -f; }
    set -u
    stage boot
    mount -t proc proc /proc
    mount -t sysfs sysfs /sys
    mountpoint -q /dev || mount -t devtmpfs devtmpfs /dev
    mkdir -p /dev/pts /dev/shm
    mount -t devpts -o gid=5,mode=620,ptmxmode=666 devpts /dev/pts
    mount -t tmpfs -o mode=1777 tmpfs /dev/shm
    mount -t cgroup2 cgroup2 /sys/fs/cgroup
    mount -t tmpfs -o mode=755 tmpfs /run
    mount -t tmpfs -o mode=1777 tmpfs /tmp
    mount -t tmpfs -o mode=700 tmpfs /root
    mkdir -p /run/tsugumi
    # A writable /etc: tsugumi-vmd adds one user per project.
    cp -a /etc /run/etc && mount --bind /run/etc /etc
    echo "kernel $(uname -r), $(nproc) CPUs, $(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo) MiB"

    # The Nix store disk: formatted on first boot, then mounted as /persist.
    stage store
    if ! blkid -p /dev/vdb >/dev/null 2>&1; then
      echo "formatting the store disk ($(($(blockdev --getsize64 /dev/vdb) >> 30)) GiB)"
      mkfs.ext4 -q -F -L tsugumi-store /dev/vdb || fail "cannot format the store disk"
    fi
    e2fsck -p /dev/vdb >/dev/null 2>&1 || true
    mount -t ext4 -o noatime /dev/vdb /persist || fail "cannot mount the store disk"
    df -h /persist | tail -1

    # The image's store is the lower layer; builds land on the store disk.
    stage nix
    mkdir -p /persist/nix/upper /persist/nix/work
    if [ -d /persist/nix/upper/store ]; then
      for f in /persist/nix/upper/store/*; do
        if [ -c "$f" ] && [ -e "/nix/store/''${f##*/}" ]; then rm -f "$f"; fi
      done
    fi
    mount -t overlay overlay -o lowerdir=/nix,upperdir=/persist/nix/upper,workdir=/persist/nix/work,index=off,redirect_dir=off,metacopy=off,xino=off /nix \
      || fail "cannot mount the Nix store overlay"
    # /registration and /store-paths: this closure (closureInfo), copied by the image build.
    nix-store --load-db < /registration || fail "cannot register the image store"
    mkdir -p /nix/var/nix/gcroots/tsugumi/projects /nix/var/nix/daemon-socket
    # The VM's own closure is a GC root: CacheClear removes Card builds only.
    while read -r p; do ln -sfn "$p" "/nix/var/nix/gcroots/tsugumi/''${p##*/}"; done < /store-paths
    chmod 0755 /nix/var/nix/gcroots/tsugumi/projects
    nix-daemon --daemon >/persist/nix-daemon.log 2>&1 &
    for i in $(seq 100); do [ -S /nix/var/nix/daemon-socket/socket ] && break; sleep 0.1; done
    [ -S /nix/var/nix/daemon-socket/socket ] || fail "nix-daemon did not start (see /persist/nix-daemon.log)"
    echo "nix-daemon ready; store $(du -sh /persist/nix/upper 2>/dev/null | cut -f1) on the store disk"

    # Open projects: one virtiofs device each (tsugumi-<slot>), mounted by
    # tsugumi-vmd under /mnt/slots while the project runs.
    mount -t tmpfs -o mode=755 tmpfs /mnt

    stage network
    ip() { ${busybox}/bin/ip "$@"; }
    ip link set lo up
    ${busybox}/bin/udhcpc -i eth0 -s ${udhcpcScript} -q -f -n -t 20 -T 1 || echo "no network (DHCP failed); Nix downloads will fail"

    stage supervisor
    exec ${tini}/bin/tini -- ${tsugumid}/bin/tsugumi-vmd \
      --tsugumid ${tsugumid}/bin/tsugumid --tini ${tini}/bin/tini --bash ${bashInteractive}/bin/bash \
      --path ${lib.makeBinPath projectTools} --cert-file ${cacert}/etc/ssl/certs/ca-bundle.crt
  '';
  runtime = [ tsugumid nix cacert bashInteractive coreutils util-linux e2fsprogs busybox tini etc init ] ++ projectTools;
  registration = closureInfo { rootPaths = runtime; };
in
runCommand "tsugumi-vm-${tsugumid.version}" { nativeBuildInputs = [ erofs-utils ]; } ''
  root=$TMPDIR/root
  mkdir -p $root/nix/store $root/{proc,sys,dev,run,tmp,root,persist,project,control,history,var/empty,bin,mnt}
  for p in $(cat ${registration}/store-paths); do cp -a "$p" $root/nix/store/; done
  cp ${registration}/registration $root/registration
  cp ${registration}/store-paths $root/store-paths
  cp -rL ${etc}/etc $root/etc
  chmod -R u+w $root/etc
  ln -s /run/resolv.conf $root/etc/resolv.conf
  ln -s ${bashInteractive}/bin/sh $root/bin/sh
  ln -s ${init} $root/init
  mkdir -p $out
  mkfs.erofs -zlz4hc -T0 --all-root $out/rootfs.erofs $root
  cp ${kernel}/vmlinux $out/vmlinux
''
