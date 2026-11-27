# `nix run .#dev` (macOS): builds Tsugumi Dev from the working tree and opens
# it, for trying a change by hand.
#
#   nix run .#dev                 guest image, build, then quit a running Dev and open the new one
#   nix run .#dev -- --no-open    stop after the build (dist/Tsugumi.app)
#
# The guest image comes from `nix run .#guest-image` (an Actions artifact of
# the same identity; push the change first if there is none). The app is
# built by ui/macos/build-app.sh with Apple's toolchain, signed with your
# "Apple Development" certificate when you have one, so that macOS keeps
# treating every rebuild as the same app (AGENTS.md, "Running the Mac app").
# Dev keeps its own state in ~/Library/Application Support/Tsugumi-dev; the
# installed release is not touched.
{
  writeShellApplication,
  git,
}:
writeShellApplication {
  name = "tsugumi-dev";
  runtimeInputs = [ git ];
  text = ''
    open_app=1
    for arg in "$@"; do
      case "$arg" in
      --no-open) open_app=0 ;;
      *)
        echo "usage: nix run .#dev -- [--no-open]" >&2
        exit 2
        ;;
      esac
    done
    repo="$(git rev-parse --show-toplevel)"
    if [ ! -x "$repo/ui/macos/build-app.sh" ]; then
      echo "dev: run this inside the Tsugumi repository" >&2
      exit 1
    fi
    cd "$repo"

    nix run "$repo#guest-image"

    if [ -z "''${TSUGUMI_CODESIGN_IDENTITY:-}" ] &&
      /usr/bin/security find-identity -v -p codesigning 2>/dev/null | grep -q "Apple Development"; then
      export TSUGUMI_CODESIGN_IDENTITY="Apple Development"
    fi
    "$repo/ui/macos/build-app.sh"

    [ "$open_app" = 1 ] || exit 0
    # A Dev already running (from /Applications or an earlier build) has the
    # same bundle id; quit it so that the new build is the one that opens.
    # The release app has another bundle id and is left alone.
    bundle=app.tsugumi.Tsugumi.dev
    running() {
      [ "$(/usr/bin/osascript -e "application id \"$bundle\" is running" 2>/dev/null)" = true ]
    }
    if running; then
      /usr/bin/osascript -e "tell application id \"$bundle\" to quit" >/dev/null 2>&1 || true
      for _ in $(seq 1 50); do
        running || break
        sleep 0.2
      done
    fi
    /usr/bin/open "$repo/dist/Tsugumi.app"
    echo "dev: opened $repo/dist/Tsugumi.app"
  '';
}
