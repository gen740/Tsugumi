#!/usr/bin/env bash
# Builds dist/tsugumi-macos-arm64.tar.gz and its checksum, what cli/install.sh
# installs on a Mac: the server of web/ with Node.js, tsugumi-vmhost, the shared
# VM's boot disk and the source the VM builds Tsugumi from (the same two as the
# app: dist/tsugumi-vm, macOS/Container/vm-source.txt).
#
#   cli/package-macos.sh
#
# tsugumi-vmhost is signed ad-hoc with the virtualization entitlement, unless
# TSUGUMI_CODESIGN_IDENTITY names a certificate.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
dist="$repo/dist"
tree="$dist/cli/tsugumi"
source "$repo/cli/node-version.env"

vm="$dist/tsugumi-vm"
[ -f "$vm/vmlinux" ] && [ -f "$vm/rootfs.erofs" ] || "$repo/macOS/Container/build-image.sh" --vm

# The Web UI, built from the pinned packages.
nix develop "$repo" -c bash -c "cd '$repo/web' && npm ci --ignore-scripts && npm run gen && npm run build"

"$repo/macOS/tools/build-agent.sh"

# As for the app: Apple's toolchain in a clean environment, only flatc from Nix.
flatc_bin="$("$repo/macOS/tools/flatc-bin.sh")"
swiftenv=(env -i HOME="$HOME" TSUGUMI_CLIENT_LIB_DIR="$repo/build-host/client" PATH="$flatc_bin:/usr/bin:/bin:/usr/sbin:/sbin")
build_opts=(-c release --build-system swiftbuild --arch arm64)
(cd "$repo/macOS" && "${swiftenv[@]}" swift build "${build_opts[@]}" --product tsugumi-vmhost)
bin="$(cd "$repo/macOS" && "${swiftenv[@]}" swift build "${build_opts[@]}" --show-bin-path)"

rm -rf "$dist/cli"
mkdir -p "$tree/bin" "$tree/web" "$tree/licenses"
cp "$repo/VERSION" "$tree/"
install -m 755 "$repo/cli/tsugumi-macos" "$tree/tsugumi"

# Node.js: the official build, checked against the pinned checksum.
node_tar="node-v$NODE_VERSION-darwin-arm64.tar.gz"
cache="$dist/cli-cache"
mkdir -p "$cache"
[ -f "$cache/$node_tar" ] || curl -fsSL -o "$cache/$node_tar" "https://nodejs.org/dist/v$NODE_VERSION/$node_tar"
echo "$NODE_SHA256_DARWIN_ARM64  $cache/$node_tar" | shasum -a 256 -c - >/dev/null
tar -xzf "$cache/$node_tar" -C "$tree/bin" --strip-components=2 "node-v$NODE_VERSION-darwin-arm64/bin/node"
tar -xzf "$cache/$node_tar" -C "$tree/licenses" --strip-components=1 "node-v$NODE_VERSION-darwin-arm64/LICENSE"
mv "$tree/licenses/LICENSE" "$tree/licenses/node.LICENSE"

cp "$bin/tsugumi-vmhost" "$repo/build-host/core/tsugumi-agentd" "$repo/build-host/client/tsugumi-rpc" "$tree/bin/"

codesign --force --sign "${TSUGUMI_CODESIGN_IDENTITY:--}" "$tree/bin/tsugumi-agentd"
codesign --force --sign "${TSUGUMI_CODESIGN_IDENTITY:--}" "$tree/bin/tsugumi-rpc"
codesign --force --sign "${TSUGUMI_CODESIGN_IDENTITY:--}" --entitlements "$repo/macOS/Resources/Tsugumi.entitlements" "$tree/bin/tsugumi-vmhost"

# The server finds these from web/dist/server.js, as in the repository.
cp -R "$repo/web/dist" "$tree/web/dist"
find "$tree/web/dist" -name '*.map' -delete
cp -R "$repo/demos" "$tree/demos"
cp "$repo/flake.lock" "$tree/"

cp -R "$vm" "$tree/vm"
src="$tree/vm-source"
mkdir -p "$src"
grep -v '^#' "$repo/macOS/Container/vm-source.txt" | while read -r entry; do
  [ -n "$entry" ] || continue
  mkdir -p "$src/$(dirname "$entry")"
  cp -R "$repo/$entry" "$src/$entry"
done

# Licenses: ours, the packages bundled into the server and the page, FlatBuffers
# in tsugumi-vmhost, and the boot disk's (from its release, publish-vm.sh).
cp "$repo/LICENSE" "$repo/THIRD_PARTY_NOTICES.md" "$tree/licenses/"
nix develop "$repo" -c node "$repo/web/scripts/licenses.mjs" --all "$tree/licenses/npm"
mkdir -p "$tree/licenses/swift-packages/flatbuffers"
cp "$repo/macOS/.build/checkouts/flatbuffers/LICENSE"* "$tree/licenses/swift-packages/flatbuffers/"
vm_licenses="https://github.com/${TSUGUMI_VM_RELEASES:-gen740/tsugumi-vm}/releases/download/$(cut -d- -f1 "$vm/vm-version")/licenses.tar.gz"
curl -fsSL "$vm_licenses" | tar -xz -C "$tree/licenses" || echo "warning: no licenses of the boot disk at $vm_licenses" >&2

tar -czf "$dist/tsugumi-macos-arm64.tar.gz" -C "$dist/cli" tsugumi
(cd "$dist" && shasum -a 256 tsugumi-macos-arm64.tar.gz > tsugumi-macos-arm64.tar.gz.sha256)
rm -rf "$dist/cli"
ls -l "$dist/tsugumi-macos-arm64.tar.gz"
