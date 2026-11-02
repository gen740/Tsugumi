# Guest kernel for Tsugumi environments (Apple container VMs).
#
# The recommended kernel of Apple container 0.6.0 (6.12.28, kata based) has
# SELinux built in and as the default LSM. Its inode release hook
# (selinux_inode_free_security) dereferences NULL when a mount namespace is
# torn down while its superblock's inodes are evicted -- which happens all the
# time here (Nix build sandboxes, bubblewrap, container shutdown). The oops
# leaves the exiting process stuck, the VM never shuts down and `container
# stop` / `rm` hang. Tsugumi does not use SELinux (Cards are isolated with
# namespaces by bubblewrap), so this kernel is the same configuration
# (base-6.12.28.config, read from /proc/config.gz of the recommended kernel)
# without SELinux. It is passed with `--kernel` to Tsugumi's containers only;
# the system's default kernel is not changed.
{ lib, stdenv, fetchurl, flex, bison, bc, perl, openssl, elfutils, python3 }:
stdenv.mkDerivation rec {
  pname = "tsugumi-guest-kernel";
  version = "6.12.28";
  src = fetchurl {
    url = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${version}.tar.xz";
    hash = "sha256-6KCZGCVirs/3gd5yznaUYecG2Xr0LXQN/yDrRQ3Vdx4=";
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
      --set-str LSM "landlock,lockdown,yama,loadpin,safesetid,smack,tomoyo,apparmor,ipe,bpf"
    make $makeFlags olddefconfig
    # Fail when the result still has SELinux.
    ! grep -q '^CONFIG_SECURITY_SELINUX=y' .config
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
