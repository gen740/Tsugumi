#!/usr/bin/env bash
# Builds dist/tsugumi-macos-arm64.tar.gz and its checksum, what cli/install.sh
# installs on a Mac: the tsugumi command, hostd and OpenSSL, the built Web UI
# (hostd serves it), tsugumi-vmhost, the shared
# release-built guest archive (the same payload as the app).
#
#   cli/package-macos.sh [--release]
#
# tsugumi-vmhost is signed ad-hoc with the virtualization entitlement, unless
# TSUGUMI_CODESIGN_IDENTITY names a certificate (ignored with --release).
set -euo pipefail
# Runtime files carry no Finder resource forks or AppleDouble sidecars.
export COPYFILE_DISABLE=1
release=0
for arg in "$@"; do
  case "$arg" in
    --release) release=1 ;;
    *) echo "usage: $0 [--release]" >&2; exit 2 ;;
  esac
done
sign_identity="${TSUGUMI_CODESIGN_IDENTITY:--}"
if [ "$release" -eq 1 ]; then
  sign_identity="-"
fi
repo="$(cd "$(dirname "$0")/.." && pwd)"
dist="$repo/dist"
tree="$dist/cli/tsugumi"

vm="$dist/tsugumi-vm"
if [ "$release" -eq 1 ]; then
  (cd "$repo" && nix run .#guest-image -- --release)
else
  (cd "$repo" && nix run .#guest-image)
fi

# The Web UI, built reproducibly from the pinned Nix package.
web_out="$(cd "$repo" && nix build --no-link --print-out-paths '.#web^out')"

if [ "$release" -eq 1 ]; then
  "$repo/hostd/tools/build-macos.sh" --release
else
  "$repo/hostd/tools/build-macos.sh"
fi

# As for the app: Apple's toolchain in a clean environment, only flatc from Nix.
flatc_bin="$(cd "$repo" && nix build --no-link --print-out-paths .#flatc)/bin"
swiftenv=(env -i HOME="$HOME" TSUGUMI_CLIENT_LIB_DIR="$repo/build-host/client" PATH="$flatc_bin:/usr/bin:/bin:/usr/sbin:/sbin")
build_opts=(-c release --build-system swiftbuild --arch arm64)
(cd "$repo/vm/macos/runtime" && "${swiftenv[@]}" swift build "${build_opts[@]}" --product tsugumi-vmhost)
bin="$(cd "$repo/vm/macos/runtime" && "${swiftenv[@]}" swift build "${build_opts[@]}" --show-bin-path)"

rm -rf "$dist/cli"
mkdir -p "$tree/bin" "$tree/ui/web" "$tree/Frameworks" "$tree/Resources"
cp "$repo/VERSION" "$tree/"
mkdir -p "$tree/licenses"
cp -R "$repo/third-party/". "$tree/licenses/"
install -m 755 "$repo/cli/tsugumi-macos" "$tree/tsugumi"

cp "$bin/tsugumi-vmhost" "$repo/build-host/hostd/tsugumi-hostd" "$repo/build-host/cli/tsugumi" "$tree/bin/"
cp "$repo/build-host/Frameworks/"*.dylib "$tree/Frameworks/"
cp -R "$repo/build-host/Resources/git" "$tree/Resources/git"
cp -R "$repo/build-host/Resources/git-licenses" "$tree/licenses/git"

for library in "$tree/Frameworks/"*.dylib; do
  /usr/bin/strip -S "$library"
  codesign --force --sign "$sign_identity" "$library"
done
for tool in "$tree/Resources/git/"*; do
  codesign --force --sign "$sign_identity" "$tool"
done
for binary in "$tree/bin/"*; do
  /usr/bin/strip -S "$binary"
done
codesign --force --sign "$sign_identity" "$tree/bin/tsugumi"
codesign --force --sign "$sign_identity" "$tree/bin/tsugumi-hostd"
codesign --force --sign "$sign_identity" --entitlements "$repo/vm/macos/Resources/Tsugumi.entitlements" "$tree/bin/tsugumi-vmhost"

# hostd serves ui/web/dist/public; the command finds it under its root, as in the repository.
cp -R "$web_out/share/tsugumi/ui/web/dist" "$tree/ui/web/dist"
chmod -R u+w "$tree/ui/web/dist"
cp -R "$repo/demos" "$tree/demos"
cp "$repo/flake.lock" "$tree/"

mkdir -p "$tree/vm"
cp "$vm/tsugumi-vm.tar.gz" "$vm/vm-manifest.json" "$tree/vm/"

tar -czf "$dist/tsugumi-macos-arm64.tar.gz" -C "$dist/cli" tsugumi
(cd "$dist" && shasum -a 256 tsugumi-macos-arm64.tar.gz > tsugumi-macos-arm64.tar.gz.sha256)
rm -rf "$dist/cli"
ls -l "$dist/tsugumi-macos-arm64.tar.gz"
