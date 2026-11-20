# Release-built shared guest: Nix and the complete Tsugumi runtime closure.
# First boot registers the shipped store and starts the prebuilt supervisor.
# Card definitions may still build with Nix.
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
#   hvc0        early boot, kernel and supervisor fallback logs, with
#               "TSUGUMI-STAGE <name>" lines for the app's progress
#   vsock       1024 guest-to-host control, 1025 project RPC, 1026 terminals
#
# Contents and why each is needed:
#   nix            the one nix-daemon of the shared store, the clients, and
#                  builds of Card definitions
#   cacert         TLS for cache.nixos.org and the pinned nixpkgs archive
#   bash/coreutils/findutils/util-linux/e2fsprogs/gawk  /init (store disk,
#                  overlay)
#   busybox        udhcpc and ip for the NAT network
{
  lib,
  runCommand,
  writeShellScript,
  writeTextDir,
  closureInfo,
  buildEnv,
  erofs-utils,
  kernel,
  vm-system,
  nix,
  cacert,
  bashInteractive,
  coreutils,
  findutils,
  util-linux,
  e2fsprogs,
  busybox,
  gawk,
}:
let
  nixbld = map toString (lib.range 1 16);
  nixConf = writeTextDir "etc/nix/nix.conf" ''
    build-users-group = nixbld
    sandbox = true
    # Registered paths must survive power loss, not just their database rows.
    experimental-features = nix-command flakes
    substituters = https://cache.nixos.org
    trusted-public-keys = cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY=
    extra-substituters = https://tsugumi.cachix.org
    extra-trusted-public-keys = tsugumi.cachix.org-1:am26Q4VFWYLyJ+t+O5j03MmMVmZ2Ifw6cbNUoLhlrhI=
    !include /run/tsugumi/binary-caches/nix.conf
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
  storeHealth = writeShellScript "tsugumi-store-health" (
    "export PATH=${
      lib.makeBinPath [
        coreutils
        util-linux
        nix
        bashInteractive
      ]
    }\n"
    + builtins.readFile ./store-health.sh
  );
  init = writeShellScript "tsugumi-vm-init" ''
    export PATH=${
      lib.makeBinPath [
        coreutils
        findutils
        util-linux
        e2fsprogs
        nix
        bashInteractive
        gawk
      ]
    }
    export NIX_SSL_CERT_FILE=${cacert}/etc/ssl/certs/ca-bundle.crt
    export SSL_CERT_FILE=$NIX_SSL_CERT_FILE HOME=/root TMPDIR=/tmp USER=root
    exec </dev/console >/dev/console 2>&1
    stage() { echo "TSUGUMI-STAGE $*"; }
    # Boot failures deliberately leave the dirty marker for the next boot.
    fail() { echo "TSUGUMI-FAILED $*"; sync; ${busybox}/bin/poweroff -f; }
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
    # Hardening for code of many projects in one kernel (kernel.nix has the
    # rest, and no BPF JIT or userfaultfd at all): no kernel addresses or log
    # for users, ptrace only of descendants, no links or files planted in
    # shared sticky folders, BPF for root only.
    for kv in kernel.kptr_restrict=2 kernel.dmesg_restrict=1 kernel.perf_event_paranoid=3 \
      kernel.yama.ptrace_scope=1 kernel.unprivileged_bpf_disabled=1 fs.protected_symlinks=1 \
      fs.protected_hardlinks=1 fs.protected_fifos=2 fs.protected_regular=2 fs.suid_dumpable=0; do
      echo "''${kv#*=}" > "/proc/sys/$(echo "''${kv%%=*}" | tr . /)" 2>/dev/null || echo "sysctl $kv: not set"
    done
    # A writable /etc: tsugumi-vmd adds one user per project.
    cp -a /etc /run/etc && mount --bind /run/etc /etc
    echo "kernel $(uname -r), $(nproc) CPUs, $(awk '/MemTotal/ {print int($2/1024)}' /proc/meminfo) MiB"

    # The Nix store disk: formatted on first boot, then mounted as /persist.
    stage store
    if ! blkid -p /dev/vdb >/dev/null 2>&1; then
      echo "formatting the store disk ($(($(blockdev --getsize64 /dev/vdb) >> 30)) GiB)"
      mkfs.ext4 -q -F -L tsugumi-store /dev/vdb || fail "cannot format the store disk"
    fi
    mount -t ext4 -o noatime /dev/vdb /persist || fail "cannot mount the store disk"
    # A larger store size in Settings grows the disk file; the file system
    # follows here, online (a no-op when it already fills the disk).
    grown=$(resize2fs /dev/vdb 2>&1) || echo "cannot grow the store disk: $grown"
    case "$grown" in *"is now"*) echo "grew the store disk to $(($(blockdev --getsize64 /dev/vdb) >> 30)) GiB" ;; esac
    # Mark the disk before any store writes, including image registration.
    check_reason=$(${storeHealth} begin /persist) || fail "cannot mark the store disk"
    ln -s ${storeHealth} /run/tsugumi/store-health
    ln -s ${vm-system}/vmd /run/tsugumi/vmd
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
    # /registration and /store-paths: this closure (closureInfo), copied by the
    # image build.
    nix-store --load-db < /registration || fail "cannot register the image store"
    mkdir -p /nix/var/nix/gcroots/tsugumi/projects /nix/var/nix/daemon-socket
    # The VM's own closures are GC roots: CacheClear removes Card builds only.
    while read -r p; do ln -sfn "$p" "/nix/var/nix/gcroots/tsugumi/''${p##*/}"; done < /store-paths
    chmod 0755 /nix/var/nix/gcroots/tsugumi/projects
    # Human-configured caches are read before starting the daemon. Project
    # namespaces replace /run/tsugumi, so they cannot read cache credentials.
    # Trouble with them never stops the boot: without the snapshot nix.conf's
    # !include finds nothing, which leaves the pinned default caches and their
    # keys. That only loses downloads (Nix builds instead); it trusts no new
    # key and sends no credentials anywhere.
    mkdir -p /run/tsugumi/binary-caches
    if ! mount -t virtiofs -o ro tsugumi-binary-caches /run/tsugumi/binary-caches; then
      echo "binary caches: cannot mount the settings; starting with the default caches only"
    elif ! nix config show substituters >/dev/null 2>&1; then
      umount /run/tsugumi/binary-caches
      echo "binary caches: the settings are not valid Nix configuration; starting with the default caches only"
    fi
    # What the daemon will use, for the run records (URLs only; tsugumi-vmd
    # hands the file to every project's tsugumid).
    nix config show substituters 2>/dev/null | awk '{ for (i = 1; i <= NF; i++) print $i }' > /run/tsugumi-substituters
    chmod 0644 /run/tsugumi-substituters
    echo "binary caches: $(wc -l < /run/tsugumi-substituters) configured"
    rm -f /nix/var/nix/daemon-socket/socket
    nix-daemon --daemon >/persist/nix-daemon.log 2>&1 &
    ${vm-system}/vmd --wait-daemon "$!" /nix/var/nix/daemon-socket/socket \
      || fail "nix-daemon did not start (see /persist/nix-daemon.log)"
    echo "nix-daemon ready; store $(du -sh /persist/nix/upper 2>/dev/null | cut -f1) on the store disk"

    # Open projects: one virtiofs device each (tsugumi-<slot>), mounted by
    # tsugumi-vmd under /mnt/slots while the project runs.
    mount -t tmpfs -o mode=755 tmpfs /mnt

    stage network
    ip() { ${busybox}/bin/ip "$@"; }
    ip link set lo up
    ${busybox}/bin/udhcpc -i eth0 -s ${udhcpcScript} -q -f -t 0 || echo "no network (DHCP failed); Nix downloads will fail"

    # No project starts before repair succeeds.
    ${storeHealth} check /persist "$check_reason" \
      || fail "Nix store content check/repair failed; see the VM console and /persist/store-health/check.log"

    stage supervisor
    exec ${vm-system}/tini -- ${vm-system}/supervisor
  '';
  runtime = [
    nix
    cacert
    bashInteractive
    coreutils
    findutils
    util-linux
    e2fsprogs
    busybox
    gawk
    etc
    init
    vm-system
  ];
  registration = closureInfo { rootPaths = runtime; };
in
runCommand "tsugumi-vm-boot-1" { nativeBuildInputs = [ erofs-utils ]; } ''
  root=$TMPDIR/root
  mkdir -p $root/nix/store $root/{proc,sys,dev,run,tmp,root,persist,project,control,scratch,var/empty,bin,mnt}
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
