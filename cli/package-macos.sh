#!/usr/bin/env bash
# Builds dist/tsugumi-macos-arm64.tar.gz and its checksum, what cli/install.sh
# installs on a Mac: the server of ui/web/ with Node.js, hostd and OpenSSL,
# tsugumi-vmhost, the shared
# release-built guest archive (the same payload as the app).
#
#   cli/package-macos.sh
#
# tsugumi-vmhost is signed ad-hoc with the virtualization entitlement, unless
# TSUGUMI_CODESIGN_IDENTITY names a certificate.
set -euo pipefail
# Runtime files carry no Finder resource forks or AppleDouble sidecars.
export COPYFILE_DISABLE=1
repo="$(cd "$(dirname "$0")/.." && pwd)"
dist="$repo/dist"
tree="$dist/cli/tsugumi"
source "$repo/cli/node-version.env"

vm="$dist/tsugumi-vm"
(cd "$repo" && nix run .#guest-image)

# The Web UI, built from the pinned packages.
nix develop "$repo" -c bash -c "cd '$repo/ui/web' && npm ci --ignore-scripts && npm run gen && npm run build"

"$repo/hostd/tools/build-macos.sh"

# As for the app: Apple's toolchain in a clean environment, only flatc from Nix.
flatc_bin="$(cd "$repo" && nix build --no-link --print-out-paths .#flatc)/bin"
swiftenv=(env -i HOME="$HOME" TSUGUMI_CLIENT_LIB_DIR="$repo/build-host/client" PATH="$flatc_bin:/usr/bin:/bin:/usr/sbin:/sbin")
build_opts=(-c release --build-system swiftbuild --arch arm64)
(cd "$repo/vm/macos/runtime" && "${swiftenv[@]}" swift build "${build_opts[@]}" --product tsugumi-vmhost)
bin="$(cd "$repo/vm/macos/runtime" && "${swiftenv[@]}" swift build "${build_opts[@]}" --show-bin-path)"

rm -rf "$dist/cli"
mkdir -p "$tree/bin" "$tree/ui/web" "$tree/Frameworks"
cp "$repo/VERSION" "$tree/"
mkdir -p "$tree/licenses"
cp -R "$repo/third-party/". "$repo/cli/third-party/node" "$tree/licenses/"
install -m 755 "$repo/cli/tsugumi-macos" "$tree/tsugumi"

# Node.js: the official build, checked against the pinned checksum.
node_tar="node-v$NODE_VERSION-darwin-arm64.tar.gz"
cache="$dist/cli-cache"
mkdir -p "$cache"
[ -f "$cache/$node_tar" ] || curl -fsSL -o "$cache/$node_tar" "https://nodejs.org/dist/v$NODE_VERSION/$node_tar"
echo "$NODE_SHA256_DARWIN_ARM64  $cache/$node_tar" | shasum -a 256 -c - >/dev/null
tar -xzf "$cache/$node_tar" -C "$tree/bin" --strip-components=2 "node-v$NODE_VERSION-darwin-arm64/bin/node"

cp "$bin/tsugumi-vmhost" "$repo/build-host/hostd/tsugumi-hostd" "$tree/bin/"
cp "$repo/build-host/Frameworks/"*.dylib "$tree/Frameworks/"

for library in "$tree/Frameworks/"*.dylib; do
  /usr/bin/strip -S "$library"
  codesign --force --sign "${TSUGUMI_CODESIGN_IDENTITY:--}" "$library"
done
for binary in "$tree/bin/"*; do
  /usr/bin/strip -S "$binary"
done
codesign --force --sign "${TSUGUMI_CODESIGN_IDENTITY:--}" "$tree/bin/node"
codesign --force --sign "${TSUGUMI_CODESIGN_IDENTITY:--}" "$tree/bin/tsugumi-hostd"
codesign --force --sign "${TSUGUMI_CODESIGN_IDENTITY:--}" --entitlements "$repo/vm/macos/Resources/Tsugumi.entitlements" "$tree/bin/tsugumi-vmhost"

# The server finds these from ui/web/dist/server.js, as in the repository.
cp -R "$repo/ui/web/dist" "$tree/ui/web/dist"
cp -R "$repo/demos" "$tree/demos"
cp "$repo/flake.lock" "$tree/"

mkdir -p "$tree/vm"
cp "$vm/tsugumi-vm.tar.gz" "$vm/vm-manifest.json" "$tree/vm/"

tar -czf "$dist/tsugumi-macos-arm64.tar.gz" -C "$dist/cli" tsugumi
(cd "$dist" && shasum -a 256 tsugumi-macos-arm64.tar.gz > tsugumi-macos-arm64.tar.gz.sha256)
rm -rf "$dist/cli"
ls -l "$dist/tsugumi-macos-arm64.tar.gz"
