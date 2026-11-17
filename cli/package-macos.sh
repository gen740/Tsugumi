#!/usr/bin/env bash
# Builds dist/tsugumi-macos-arm64.tar.gz and its checksum, what cli/install.sh
# installs on a Mac: the server of web/ with Node.js, hostd and OpenSSL,
# tsugumi-vmhost, the shared
# release-built guest archive (the same payload as the app).
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
"$repo/macOS/Container/build-image.sh" --vm

# The Web UI, built from the pinned packages.
nix develop "$repo" -c bash -c "cd '$repo/web' && npm ci --ignore-scripts && npm run gen && npm run build"

"$repo/macOS/tools/build-host.sh"

# As for the app: Apple's toolchain in a clean environment, only flatc from Nix.
flatc_bin="$("$repo/macOS/tools/flatc-bin.sh")"
swiftenv=(env -i HOME="$HOME" TSUGUMI_CLIENT_LIB_DIR="$repo/build-host/client" PATH="$flatc_bin:/usr/bin:/bin:/usr/sbin:/sbin")
build_opts=(-c release --build-system swiftbuild --arch arm64)
(cd "$repo/macOS" && "${swiftenv[@]}" swift build "${build_opts[@]}" --product tsugumi-vmhost)
bin="$(cd "$repo/macOS" && "${swiftenv[@]}" swift build "${build_opts[@]}" --show-bin-path)"

rm -rf "$dist/cli"
mkdir -p "$tree/bin" "$tree/web" "$tree/licenses" "$tree/Frameworks"
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

cp "$bin/tsugumi-vmhost" "$repo/build-host/core/tsugumi-hostd" "$tree/bin/"
cp "$repo/build-host/Frameworks/"*.dylib "$tree/Frameworks/"

for library in "$tree/Frameworks/"*.dylib; do
  codesign --force --sign "${TSUGUMI_CODESIGN_IDENTITY:--}" "$library"
done
codesign --force --sign "${TSUGUMI_CODESIGN_IDENTITY:--}" "$tree/bin/tsugumi-hostd"
codesign --force --sign "${TSUGUMI_CODESIGN_IDENTITY:--}" --entitlements "$repo/macOS/Resources/Tsugumi.entitlements" "$tree/bin/tsugumi-vmhost"

# The server finds these from web/dist/server.js, as in the repository.
cp -R "$repo/web/dist" "$tree/web/dist"
find "$tree/web/dist" -name '*.map' -delete
cp -R "$repo/demos" "$tree/demos"
cp "$repo/flake.lock" "$tree/"

mkdir -p "$tree/vm"
cp "$vm/tsugumi-vm.tar.gz" "$vm/vm-manifest.json" "$tree/vm/"

# Licenses: ours, the packages bundled into the server and the page, FlatBuffers
# in tsugumi-vmhost, OpenSSL, and the complete guest closure.
cp "$repo/LICENSE" "$repo/THIRD_PARTY_NOTICES.md" "$tree/licenses/"
nix develop "$repo" -c node "$repo/web/scripts/licenses.mjs" --all "$tree/licenses/npm"
mkdir -p "$tree/licenses/openssl"
openssl_source="$(nix build --no-link --print-out-paths --inputs-from "$repo" nixpkgs#openssl.src)"
openssl_license="$(tar -tf "$openssl_source" | grep -E '^[^/]+/LICENSE.txt$')"
[ -n "$openssl_license" ] || { echo "OpenSSL license is missing" >&2; exit 1; }
tar -xOf "$openssl_source" "$openssl_license" > "$tree/licenses/openssl/LICENSE.txt"
mkdir -p "$tree/licenses/swift-packages/flatbuffers"
cp "$repo/macOS/.build/checkouts/flatbuffers/LICENSE"* "$tree/licenses/swift-packages/flatbuffers/"
tar -xzf "$vm/vm-licenses.tar.gz" -C "$tree/licenses"

tar -czf "$dist/tsugumi-macos-arm64.tar.gz" -C "$dist/cli" tsugumi
(cd "$dist" && shasum -a 256 tsugumi-macos-arm64.tar.gz > tsugumi-macos-arm64.tar.gz.sha256)
rm -rf "$dist/cli"
ls -l "$dist/tsugumi-macos-arm64.tar.gz"
