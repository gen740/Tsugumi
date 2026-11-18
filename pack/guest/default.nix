# The guest of exported apps and of tsugumi-pack itself (aarch64-linux).
#
#   $out/vmlinux       the shared VM's kernel (vm/guest/kernel.nix)
#   $out/rootfs.erofs  /init, Nix, tsugumid and what export and run need
#   $out/identity      names this image (its /init); an app records it
#
# One image, two modes, chosen on the kernel command line (tsugumi.pack=):
#
#   export  network on; /dev/vdb is the exporter's store disk (ext4, kept
#           between exports); virtiofs "pack" holds project/ and receives out/
#           (payload.erofs). See export.sh.
#   run     no network device at all; /dev/vdb is the app's payload.erofs, its
#           store over the image's (overlay); /dev/vdc is a scratch disk for
#           the store's writes and the run's work folder; virtiofs
#           "pack" holds request.json and receives result/. See run.py.
#
# The console (hvc0) carries the log, with "PACK-STAGE <name>" lines for the
# Mac side's progress. The guest powers off when done; /pack/<mode>.status
# holds the exit status.
{
  lib,
  runCommand,
  writeShellScript,
  writeTextDir,
  closureInfo,
  buildEnv,
  erofs-utils,
  kernel,
  tsugumid,
  cardNixpkgs,
  flatbuffers,
  nix,
  cacert,
  bashInteractive,
  coreutils,
  findutils,
  gnugrep,
  gnused,
  gnutar,
  gzip,
  util-linux,
  e2fsprogs,
  busybox,
  gawk,
  jq,
  python3Minimal,
}:
let
  nixbld = map toString (lib.range 1 16);
  # The RPC schema tsugumi-rpc encodes requests with.
  schema = runCommand "tsugumi-rpc-schema" { nativeBuildInputs = [ flatbuffers ]; } ''
    mkdir -p $out
    flatc -b --schema --bfbs-builtins -I ${tsugumid.src}/protocol -o $out ${tsugumid.src}/protocol/tsugumi.fbs
  '';
  etc = buildEnv {
    name = "tsugumi-pack-etc";
    paths = [
      (writeTextDir "etc/nix/nix.conf" ''
        build-users-group = nixbld
        sandbox = true
        experimental-features = nix-command flakes
        substituters = https://cache.nixos.org https://tsugumi.cachix.org
        trusted-public-keys = cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY= tsugumi.cachix.org-1:am26Q4VFWYLyJ+t+O5j03MmMVmZ2Ifw6cbNUoLhlrhI=
        max-jobs = auto
        allow-import-from-derivation = false
        keep-outputs = true
        keep-derivations = true
        trusted-users = root
      '')
      (writeTextDir "etc/passwd" (
        ''
          root:x:0:0:root:/root:/bin/sh
          nobody:x:65534:65534:nobody:/var/empty:/bin/false
        ''
        + lib.concatMapStrings (
          i: "nixbld${i}:x:${toString (30000 + lib.toInt i)}:30000:nixbld:/var/empty:/bin/false\n"
        ) nixbld
      ))
      (writeTextDir "etc/group" ''
        root:x:0:
        nixbld:x:30000:${lib.concatMapStringsSep "," (i: "nixbld${i}") nixbld}
        nobody:x:65534:
      '')
      (writeTextDir "etc/hosts" "127.0.0.1 localhost\n")
      (writeTextDir "etc/nsswitch.conf" "hosts: files dns\n")
    ];
  };
  tools = [
    nix
    bashInteractive
    coreutils
    findutils
    gnugrep
    gnused
    gnutar
    gzip
    util-linux
    e2fsprogs
    gawk
    jq
    erofs-utils
    python3Minimal
  ];
  udhcpc = writeShellScript "tsugumi-pack-udhcpc" ''
    PATH=${busybox}/bin
    case "$1" in
      deconfig) ip addr flush dev "$interface"; ip link set "$interface" up ;;
      bound|renew)
        ip addr flush dev "$interface"
        ip addr add "$ip/$mask" dev "$interface"
        for r in $router; do ip route add default via "$r" dev "$interface"; break; done
        : > /run/resolv.conf
        for d in $dns; do echo "nameserver $d" >> /run/resolv.conf; done
        ;;
    esac
  '';
  init = writeShellScript "tsugumi-pack-init" ''
    export PATH=${lib.makeBinPath tools}
    export NIX_SSL_CERT_FILE=${cacert}/etc/ssl/certs/ca-bundle.crt
    export SSL_CERT_FILE=$NIX_SSL_CERT_FILE HOME=/root TMPDIR=/tmp USER=root
    export TSUGUMID=${tsugumid}/bin/tsugumid TSUGUMI_RPC=${tsugumid}/bin/tsugumi-rpc
    export TSUGUMI_SCHEMA=${schema}/tsugumi.bfbs FLATC=${flatbuffers}/bin/flatc
    export CARD_NIXPKGS_REV=${cardNixpkgs.rev} CARD_NIXPKGS_NARHASH=${cardNixpkgs.narHash}
    exec </dev/console >/dev/console 2>&1
    stage() { echo "PACK-STAGE $*"; }
    off() { sync; ${busybox}/bin/poweroff -f; }
    fail() { echo "PACK-FAILED $*"; [ -d /pack ] && echo 1 > "/pack/$mode.status"; off; }
    set -u
    mount -t proc proc /proc
    mount -t sysfs sysfs /sys
    mountpoint -q /dev || mount -t devtmpfs devtmpfs /dev
    mkdir -p /dev/pts /dev/shm
    [ -e /dev/fd ] || ln -s /proc/self/fd /dev/fd
    for i in 0:stdin 1:stdout 2:stderr; do [ -e /dev/''${i#*:} ] || ln -s /proc/self/fd/''${i%%:*} /dev/''${i#*:}; done
    mount -t devpts -o gid=5,mode=620,ptmxmode=666 devpts /dev/pts
    mount -t tmpfs -o mode=1777 tmpfs /dev/shm
    mount -t cgroup2 cgroup2 /sys/fs/cgroup
    mount -t tmpfs -o mode=755 tmpfs /run
    mount -t tmpfs -o mode=1777 tmpfs /tmp
    mount -t tmpfs -o mode=700 tmpfs /root
    cp -a /etc /run/etc && mount --bind /run/etc /etc
    mode=$(sed -n 's/.*tsugumi\.pack=\([a-z]*\).*/\1/p' /proc/cmdline)
    mount -t virtiofs pack /pack || fail "cannot mount the pack folder"
    stage boot "$mode" "$(nproc) CPUs" "$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo) MiB"

    # /nix: the image's store, under the payload's (run) or with the
    # exporter's store disk as the writable layer (export).
    mount -t tmpfs -o mode=755 tmpfs /mnt
    lower=/nix
    case "$mode" in
    export)
      if ! blkid -p /dev/vdb >/dev/null 2>&1; then
        mkfs.ext4 -q -F -L tsugumi-pack-store /dev/vdb || fail "cannot format the store disk"
      fi
      mount -t ext4 -o noatime /dev/vdb /persist || fail "cannot mount the store disk"
      upper=/persist/nix ;;
    run)
      mkdir -p /mnt/payload
      mount -t erofs -o ro /dev/vdb /mnt/payload || fail "cannot mount the payload"
      lower=/mnt/payload/nix:/nix
      # /dev/vdc: this run's scratch disk (sparse, removed by the app after
      # the run). tsugumid keeps run scratch on a disk, not in memory.
      mkfs.ext4 -q -F -E lazy_itable_init=1,lazy_journal_init=1 /dev/vdc || fail "cannot format the scratch disk"
      mount -t ext4 -o noatime /dev/vdc /persist || fail "cannot mount the scratch disk"
      mkdir -p /persist/work
      mount --bind /persist/work /work
      upper=/persist/nix ;;
    *) fail "unknown mode '$mode'" ;;
    esac
    mkdir -p $upper/upper $upper/work
    mount -t overlay overlay -o lowerdir=$lower,upperdir=$upper/upper,workdir=$upper/work,index=off,redirect_dir=off,metacopy=off,xino=off /nix \
      || fail "cannot mount the Nix store"
    nix-store --load-db < /registration || fail "cannot register the image store"
    if [ "$mode" = run ]; then
      nix-store --load-db < /mnt/payload/registration || fail "cannot register the payload"
    fi
    mkdir -p /nix/var/nix/gcroots /nix/var/nix/daemon-socket
    nix-daemon --daemon >/run/nix-daemon.log 2>&1 &
    for i in $(seq 100); do [ -S /nix/var/nix/daemon-socket/socket ] && break; sleep 0.05; done
    [ -S /nix/var/nix/daemon-socket/socket ] || fail "nix-daemon did not start"
    export NIX_REMOTE=daemon
    ${busybox}/bin/ip link set lo up

    status=0
    case "$mode" in
    export)
      stage network
      ${busybox}/bin/udhcpc -i eth0 -s ${udhcpc} -q -f -n -t 20 -T 1 >/dev/null || fail "no network (DHCP failed)"
      bash ${./export.sh} 2>&1 | tee /pack/export.log
      status=''${PIPESTATUS[0]} ;;
    run)
      mkdir -p /pack/result
      cp -a /mnt/payload/project /work/project
      chmod -R u+w /work/project
      PACK_WORK=/work python3 ${./run.py} /work/project /pack/request.json /pack/result 2>&1 | tee /pack/result/run.log
      status=''${PIPESTATUS[0]} ;;
    esac
    echo "$status" > "/pack/$mode.status"
    stage done "$status"
    off
  '';
  runtime = tools ++ [
    tsugumid
    schema
    flatbuffers
    cacert
    busybox
    etc
    init
  ];
  registration = closureInfo { rootPaths = runtime; };
in
runCommand "tsugumi-pack-guest" { nativeBuildInputs = [ erofs-utils ]; } ''
  root=$TMPDIR/root
  mkdir -p $root/nix/store $root/{proc,sys,dev,run,tmp,root,persist,pack,work,var/empty,bin,mnt}
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
  echo ${baseNameOf (toString init)} > $out/identity
''
