# The guests of tsugumi-pack and of the apps it exports (aarch64-linux).
#
#   $out/vmlinux       the shared VM's kernel (vm/guest/kernel.nix)
#   $out/export.erofs  the exporter: Nix and tsugumid, network on
#   $out/run.erofs     an app's runner: tsugumid and its run tools, no Nix
#   $out/identity      names this pair (the runner's /init); an app records it
#
# export  /dev/vdb is the exporter's store disk (ext4, kept between exports);
#         virtiofs "pack" holds project/ and receives out/ (payload.erofs).
#         Every Card is evaluated and built here, and only here. See export.sh.
# run     no network device at all; /dev/vdb is the app's payload.erofs, laid
#         over the image's store (overlay); /dev/vdc is a scratch disk for the
#         run's work folder; virtiofs "pack" holds request.json and receives
#         result/. tsugumid runs the builds prepared at export and evaluates
#         nothing, so this image has no Nix, nixpkgs or derivations. See run.py.
#
# Both use /work as the work folder: the Cards' evaluation keys name folders
# under /work/scratch, and a run checks each Card's key against the export's.
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
    cp ${tsugumid.src}/protocol/runtime_state.fbs $out/
  '';
  baseEtc = [
    (writeTextDir "etc/hosts" "127.0.0.1 localhost\n")
    (writeTextDir "etc/nsswitch.conf" "hosts: files dns\n")
  ];
  runEtc = buildEnv {
    name = "tsugumi-pack-run-etc";
    paths = baseEtc ++ [
      (writeTextDir "etc/passwd" ''
        root:x:0:0:root:/root:/bin/sh
        nobody:x:65534:65534:nobody:/var/empty:/bin/false
      '')
      (writeTextDir "etc/group" ''
        root:x:0:
        nobody:x:65534:
      '')
    ];
  };
  exportEtc = buildEnv {
    name = "tsugumi-pack-export-etc";
    paths = baseEtc ++ [
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
    ];
  };
  runTools = [
    bashInteractive
    coreutils
    util-linux
    e2fsprogs
    gnused
    gawk
    python3Minimal
  ];
  exportTools = runTools ++ [
    nix
    findutils
    gnugrep
    gnutar
    gzip
    jq
    erofs-utils
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
  # What both modes do before and after their own part.
  boot = mode: tools: body: ''
    export PATH=${lib.makeBinPath tools}
    export HOME=/root TMPDIR=/tmp USER=root
    export TSUGUMID=${tsugumid}/bin/tsugumid TSUGUMI_RPC=${tsugumid}/bin/tsugumi-rpc
    export TSUGUMI_SCHEMA=${schema}/tsugumi.bfbs TSUGUMI_STATE_SCHEMA=${schema}/runtime_state.fbs
    export FLATC=${flatbuffers}/bin/flatc
    exec </dev/console >/dev/console 2>&1
    mode=${mode}
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
    mount -t tmpfs -o mode=755 tmpfs /mnt
    cp -a /etc /run/etc && mount --bind /run/etc /etc
    mount -t virtiofs pack /pack || fail "cannot mount the pack folder"
    ${busybox}/bin/ip link set lo up
    stage boot "$mode" "$(nproc) CPUs" "$(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo) MiB"
    status=0
    ${body}
    echo "$status" > "/pack/$mode.status"
    stage done "$status"
    off
  '';
  exportInit = writeShellScript "tsugumi-pack-export-init" (
    boot "export" exportTools ''
      export NIX_SSL_CERT_FILE=${cacert}/etc/ssl/certs/ca-bundle.crt SSL_CERT_FILE=${cacert}/etc/ssl/certs/ca-bundle.crt
      # What an app's runner image already holds: the payload leaves it out.
      export RUN_STORE_PATHS=${runRegistration}/store-paths
      if ! blkid -p /dev/vdb >/dev/null 2>&1; then
        mkfs.ext4 -q -F -L tsugumi-pack-store /dev/vdb || fail "cannot format the store disk"
      fi
      mount -t ext4 -o noatime /dev/vdb /persist || fail "cannot mount the store disk"
      mkdir -p /persist/nix/upper /persist/nix/work /persist/export
      mount -t overlay overlay -o lowerdir=/nix,upperdir=/persist/nix/upper,workdir=/persist/nix/work,index=off,redirect_dir=off,metacopy=off,xino=off /nix \
        || fail "cannot mount the Nix store"
      mount --bind /persist/export /work
      nix-store --load-db < /registration || fail "cannot register the image store"
      mkdir -p /nix/var/nix/gcroots /nix/var/nix/daemon-socket
      nix-daemon --daemon >/run/nix-daemon.log 2>&1 &
      for i in $(seq 100); do [ -S /nix/var/nix/daemon-socket/socket ] && break; sleep 0.05; done
      [ -S /nix/var/nix/daemon-socket/socket ] || fail "nix-daemon did not start"
      export NIX_REMOTE=daemon
      stage network
      ${busybox}/bin/udhcpc -i eth0 -s ${udhcpc} -q -f -n -t 20 -T 1 >/dev/null || fail "no network (DHCP failed)"
      bash ${./export.sh} 2>&1 | tee /pack/export.log
      status=''${PIPESTATUS[0]}
    ''
  );
  runInit = writeShellScript "tsugumi-pack-run-init" (
    boot "run" runTools ''
      mkdir -p /mnt/payload
      mount -t erofs -o ro /dev/vdb /mnt/payload || fail "cannot mount the payload"
      mount -t overlay overlay -o ro,lowerdir=/mnt/payload/nix:/nix /nix || fail "cannot mount the store"
      # /dev/vdc: this run's scratch disk (sparse, removed by the app after
      # the run). tsugumid keeps run scratch on a disk, not in memory.
      mkfs.ext4 -q -F -E lazy_itable_init=1,lazy_journal_init=1 /dev/vdc || fail "cannot format the scratch disk"
      mount -t ext4 -o noatime /dev/vdc /work || fail "cannot mount the scratch disk"
      mkdir -p /pack/result
      cp -a /mnt/payload/project /work/project
      chmod -R u+w /work/project
      PACK_WORK=/work PACK_PREPARED=/mnt/payload/cards.bin PACK_PARAMETERS=/mnt/payload/parameters.json \
        python3 ${./run.py} /work/project /pack/request.json /pack/result 2>&1 | tee /pack/result/run.log
      status=''${PIPESTATUS[0]}
    ''
  );
  base = [
    tsugumid
    schema
    flatbuffers
    busybox
  ];
  runRegistration = closureInfo { rootPaths = base ++ runTools ++ [ runEtc ]; };
  runImage = closureInfo {
    rootPaths =
      base
      ++ runTools
      ++ [
        runEtc
        runInit
      ];
  };
  exportImage = closureInfo {
    rootPaths =
      base
      ++ exportTools
      ++ [
        cacert
        exportEtc
        exportInit
      ];
  };
  image = name: registration: etc: init: ''
    root=$TMPDIR/${name}
    mkdir -p $root/nix/store $root/{proc,sys,dev,run,tmp,root,persist,pack,work,var/empty,bin,mnt}
    for p in $(cat ${registration}/store-paths); do cp -a "$p" $root/nix/store/; done
    cp ${registration}/registration $root/registration
    cp ${registration}/store-paths $root/store-paths
    cp -rL ${etc}/etc $root/etc
    chmod -R u+w $root/etc
    ln -s /run/resolv.conf $root/etc/resolv.conf
    ln -s ${bashInteractive}/bin/sh $root/bin/sh
    ln -s ${init} $root/init
    mkfs.erofs -zlz4hc -T0 --all-root $out/${name}.erofs $root
  '';
in
runCommand "tsugumi-pack-guest" { nativeBuildInputs = [ erofs-utils ]; } ''
  mkdir -p $out
  ${image "export" exportImage exportEtc exportInit}
  ${image "run" runImage runEtc runInit}
  cp ${kernel}/vmlinux $out/vmlinux
  echo ${baseNameOf (toString runInit)} > $out/identity
''
