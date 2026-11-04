# Guest kernel of the shared Tsugumi VM (Virtualization.framework), 6.12 LTS.
#
# The configuration (kernel/base-6.12.28.config) is that of a kata based VM
# kernel, brought to this version with olddefconfig, without:
#   SELinux    its inode release hook (selinux_inode_free_security) oopsed on
#              NULL when a mount namespace was torn down while its inodes were
#              evicted -- all the time here (Nix build sandboxes, bubblewrap),
#              leaving the exiting process stuck. Cards are isolated with
#              namespaces (bubblewrap), not SELinux.
#   io_uring   a large kernel surface no Card or tool here needs.
# and with unprivileged BPF off by default (BPF_UNPRIV_DEFAULT_OFF) and Yama
# (ptrace of descendants only). BPF JIT and userfaultfd are not built in. The
# rest of the hardening is sysctls set by the VM's /init (vm-boot.nix).
{ lib, stdenv, fetchurl, flex, bison, bc, perl, openssl, elfutils, python3 }:
stdenv.mkDerivation rec {
  pname = "tsugumi-guest-kernel";
  version = "6.12.115";
  src = fetchurl {
    url = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${version}.tar.xz";
    hash = "sha256-DskP5LIEIrG2ZLd+bS3eIrQL1B5+ssrYrIc5pHScsbs=";
  };
  nativeBuildInputs = [ flex bison bc perl openssl elfutils python3 ];
  enableParallelBuilding = true;
  hardeningDisable = [ "all" ];
  postPatch = "patchShebangs scripts";
  makeFlags = [ "ARCH=arm64" "LOCALVERSION=-tsugumi" "KBUILD_BUILD_USER=tsugumi" "KBUILD_BUILD_HOST=nix" ];
  configurePhase = ''
    runHook preConfigure
    cp ${./kernel/base-6.12.28.config} .config
    chmod u+w .config
    scripts/config --disable SECURITY_SELINUX \
      --disable DEFAULT_SECURITY_SELINUX --enable DEFAULT_SECURITY_DAC \
      --set-str LSM "landlock,lockdown,yama,loadpin,safesetid,smack,tomoyo,apparmor,ipe,bpf" \
      --disable IO_URING --enable BPF_UNPRIV_DEFAULT_OFF --enable SECURITY_YAMA
    make $makeFlags olddefconfig
    # Fail when the result still has SELinux or io_uring, or allows unprivileged BPF.
    ! grep -q '^CONFIG_SECURITY_SELINUX=y' .config
    ! grep -q '^CONFIG_IO_URING=y' .config
    grep -q '^CONFIG_BPF_UNPRIV_DEFAULT_OFF=y' .config
    grep -q '^CONFIG_SECURITY_YAMA=y' .config
    runHook postConfigure
  '';
  buildPhase = ''
    runHook preBuild
    make $makeFlags -j$NIX_BUILD_CORES Image
    runHook postBuild
  '';
  installPhase = ''
    install -Dm644 arch/arm64/boot/Image $out/vmlinux
    install -Dm644 .config $out/config
  '';
  meta.platforms = [ "aarch64-linux" ];
}
