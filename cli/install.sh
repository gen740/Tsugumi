#!/bin/sh
# Installs the `tsugumi` command for this user, without root:
#
#   curl -fsSL https://github.com/gen740/Tsugumi/releases/latest/download/install.sh | sh
#
#   ~/.local/share/tsugumi/<version>.<generation>/   immutable runtimes
#   ~/.local/share/tsugumi/current      that version
#   ~/.local/bin/tsugumi                the command
#
# macOS (Apple silicon, macOS 26 or later): takes tsugumi-macos-arm64.tar.gz,
# with Node.js and the shared VM's boot disk in it. Linux (x86_64, aarch64):
# takes the source, tsugumi-linux.tar.gz, and builds the command with Nix, as
# Cards need Nix there anyway. The checksum next to each file is checked first.
# Shell configuration is left alone.
#
#   TSUGUMI_VERSION    a version to install (e.g. 0.6.0) instead of the latest
#
# `tsugumi update` runs the latest release's copy of this script.
#   TSUGUMI_INSTALL_BASE  where the files are, instead of the GitHub release
set -eu

fail() {
  echo "tsugumi のインストールを中止しました: $1" >&2
  exit 1
}

data="$HOME/.local/share/tsugumi"
bin="$HOME/.local/bin"

case "$(uname -s)/$(uname -m)" in
  Darwin/arm64)
    major="$(sw_vers -productVersion | cut -d. -f1)"
    [ "$major" -ge 26 ] || fail "macOS 26 以降が必要です（この Mac は $(sw_vers -productVersion)）。"
    artifact=tsugumi-macos-arm64.tar.gz
    ;;
  Darwin/*)
    fail "Mac は Apple silicon だけに対応しています。"
    ;;
  Linux/x86_64 | Linux/aarch64)
    command -v nix >/dev/null 2>&1 || fail "Linux では Nix が必要です。https://nixos.org/download から入れて、もう一度実行してください。"
    artifact=tsugumi-linux.tar.gz
    ;;
  *)
    fail "$(uname -s) $(uname -m) には対応していません。"
    ;;
esac

if [ -n "${TSUGUMI_INSTALL_BASE:-}" ]; then
  base="$TSUGUMI_INSTALL_BASE"
elif [ -n "${TSUGUMI_VERSION:-}" ]; then
  base="https://github.com/gen740/Tsugumi/releases/download/v$TSUGUMI_VERSION"
else
  base="https://github.com/gen740/Tsugumi/releases/latest/download"
fi

command -v curl >/dev/null 2>&1 || fail "curl が見つかりません。"
if command -v shasum >/dev/null 2>&1; then
  sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }
elif command -v sha256sum >/dev/null 2>&1; then
  sha256() { sha256sum "$1" | cut -d' ' -f1; }
else
  fail "shasum も sha256sum も見つかりません。"
fi

mkdir -p "$data" "$bin"
# Next to where it goes, so that moving it into place is a rename.
work="$(mktemp -d "$data/.install.XXXXXX")"
dest=
committed=false
cleanup() {
  rm -rf "$work"
  # A signal can arrive just after the commit rename and before assignment.
  if [ "$committed" = false ] && [ -n "$dest" ] &&
     [ "$(readlink "$data/current" 2>/dev/null || :)" != "${dest##*/}" ]; then rm -rf "$dest"; fi
  rm -f "$bin/.tsugumi.$$"
}
trap cleanup EXIT
# Ctrl-C: sh runs a trap and goes on, so stop here (the EXIT trap cleans up).
trap 'echo >&2; echo "tsugumi のインストールを中止しました。何も入れていません。" >&2; exit 130' INT TERM

echo "$base/$artifact を取得しています…"
curl -fL --progress-bar -o "$work/$artifact" "$base/$artifact" || fail "$base/$artifact を取得できませんでした。"
curl -fsSL -o "$work/$artifact.sha256" "$base/$artifact.sha256" || fail "$base/$artifact.sha256 を取得できませんでした。"
want="$(cut -d' ' -f1 "$work/$artifact.sha256")"
got="$(sha256 "$work/$artifact")"
[ -n "$want" ] && [ "$want" = "$got" ] || fail "チェックサムが一致しません（期待 $want、実際 $got）。何も入れていません。"

mkdir "$work/tree"
tar -xzf "$work/$artifact" -C "$work/tree"
tree="$work/tree/tsugumi"
[ -f "$tree/VERSION" ] || fail "$artifact の中身が想定と違います。"
version="$(cat "$tree/VERSION")"

# A generation is never overwritten, even when reinstalling the same version.
case "$version" in
  '' | *[!A-Za-z0-9._+-]* | [!A-Za-z0-9]*) fail "Invalid release version: $version" ;;
esac
dest="$(mktemp -d "$data/$version.XXXXXX")"
generation="${dest##*/}"
if [ "$artifact" = tsugumi-linux.tar.gz ]; then
  echo "Nix で tsugumi $version を組み立てています（初回は数分かかります）…"
  # Keep the out-link at its final path: moving it loses Nix's GC root.
  nix --extra-experimental-features 'nix-command flakes' --accept-flake-config build "path:$tree#cli" --out-link "$dest/result" ||
    fail "Nix でのビルドに失敗しました（上の出力を確かめてください）。"
  ln -s result/bin/tsugumi "$dest/tsugumi"
else
  mv "$tree" "$dest/runtime"
  ln -s runtime/tsugumi "$dest/tsugumi"
fi
reported="$("$dest/tsugumi" --version)" || fail "The new command does not run."
[ "$reported" = "$version" ] || fail "Version check failed: expected $version, got $reported"
printf '%s\n' "$generation" > "$dest/.installed-by-tsugumi"
# Rename over a symlink itself, never into the directory it points at.
# Both links are prepared on the same filesystem as their destination.
# Detect the utility, not the OS: Nix can put GNU mv on a Mac's PATH.
mkdir "$work/mv-dir"
ln -s mv-dir "$work/mv-to"
ln -s mv-new "$work/mv-from"
if mv -fT "$work/mv-from" "$work/mv-to" 2>/dev/null; then
  mv_link_option=-fT
else
  mv -fh "$work/mv-from" "$work/mv-to" 2>/dev/null || fail "mv cannot replace a symbolic link."
  mv_link_option=-fh
fi
[ "$(readlink "$work/mv-to")" = mv-new ] || fail "mv followed a symbolic link instead of replacing it."
replace_link() {
  mv "$mv_link_option" "$1" "$2"
}
[ ! -e "$data/current" ] || [ -L "$data/current" ] || fail "Not an installer version link: $data/current"
ln -s "$generation" "$work/current"
# Prepare the launcher before committing. An existing launcher already uses current.
launcher="$bin/.tsugumi.$$"
ln -s "$data/current/tsugumi" "$launcher"
replace_link "$launcher" "$bin/tsugumi"
replace_link "$work/current" "$data/current"
committed=true
# Retain old runtimes: a running host may open their assets later. Explicit
# uninstall, after stopping hosts, removes only generations with our marker.

echo "tsugumi $version を $bin/tsugumi に入れました。"
case ":$PATH:" in
  *":$bin:"*) echo "tsugumi を実行するとブラウザで Web UI が開きます。" ;;
  *)
    echo "$bin は PATH に入っていません（シェルの設定は変更していません）。"
    echo "$bin/tsugumi で実行するか、PATH に $bin を加えてください。"
    ;;
esac
