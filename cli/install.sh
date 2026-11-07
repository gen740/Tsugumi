#!/bin/sh
# Installs the `tsugumi` command for this user, without root:
#
#   curl -fsSL https://github.com/gen740/Tsugumi/releases/latest/download/install.sh | sh
#
#   ~/.local/share/tsugumi/<version>/   what the command runs
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
trap 'rm -rf "$work"' EXIT INT TERM

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

dest="$data/$version"
rm -rf "$dest"
if [ "$artifact" = tsugumi-linux.tar.gz ]; then
  echo "Nix で tsugumi $version を組み立てています（初回は数分かかります）…"
  mkdir -p "$dest"
  # An out-link keeps the build from Nix's garbage collection, as long as it
  # stays where Nix was told it is.
  nix --extra-experimental-features 'nix-command flakes' build "path:$tree#cli" --out-link "$dest/result" || {
    rm -rf "$dest"
    fail "Nix でのビルドに失敗しました（上の出力を確かめてください）。"
  }
  ln -s result/bin/tsugumi "$dest/tsugumi"
else
  mv "$tree" "$dest"
fi
ln -sfn "$version" "$data/current"
ln -sfn "$data/current/tsugumi" "$bin/tsugumi"
# Earlier versions: what runs now keeps its open files.
for old in "$data"/*; do
  case "$old" in
    "$dest" | "$data/current") ;;
    *) rm -rf "$old" ;;
  esac
done

echo "tsugumi $version を $bin/tsugumi に入れました。"
case ":$PATH:" in
  *":$bin:"*) echo "tsugumi を実行するとブラウザで Web UI が開きます。" ;;
  *)
    echo "$bin は PATH に入っていません（シェルの設定は変更していません）。"
    echo "$bin/tsugumi で実行するか、PATH に $bin を加えてください。"
    ;;
esac
