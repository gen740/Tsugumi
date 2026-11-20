# The Linux UI's headless check (`tsugumi-gtk-check`): its windows on GTK's
# Broadway display and, unless --no-host, a hostd the test starts in a
# temporary folder, which opens a small project through an Agent's
# ShowRequest. A project's tsugumid refuses the build sandbox's root folder
# (its run scratch must sit under private parents), so CI runs the full check
# on the runner (`nix run .#linux-ui-check`) and `checks.linux-ui` runs the
# display part.
{
  lib,
  writeShellApplication,
  coreutils,
  procps,
  gtk4,
  dejavu_fonts,
  makeFontsConf,
  linux-ui,
  core,
}:
writeShellApplication {
  name = "tsugumi-gtk-check";
  runtimeInputs = [
    coreutils
    procps
  ];
  text = ''
    host=true
    if [ "''${1:-}" = --no-host ]; then host=false; fi
    work=$(mktemp -d "''${TMPDIR:-/tmp}/tsugumi-gtk-check.XXXXXX")
    broadway=
    cleanup() {
      if [ -n "$broadway" ]; then kill "$broadway" 2>/dev/null || true; fi
      # Whatever the test's host started for its project (named by $work).
      pkill -f "$work" 2>/dev/null || true
      for _ in $(seq 100); do
        pgrep -f "$work" > /dev/null || break
        sleep 0.1
      done
      pkill -KILL -f "$work" 2>/dev/null || true
      rm -rf "$work"
    }
    trap cleanup EXIT

    # Everything the host and the project write stays in $work.
    export HOME="$work/home" XDG_RUNTIME_DIR="$work/run" TSUGUMI_RUN_SCRATCH_DIR="$work/scratch"
    export XDG_DATA_HOME="$HOME/.local/share" XDG_STATE_HOME="$HOME/.local/state"
    export XDG_CACHE_HOME="$HOME/.cache" XDG_CONFIG_HOME="$HOME/.config"
    mkdir -p "$HOME" "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
    export FONTCONFIG_FILE=${makeFontsConf { fontDirectories = [ dejavu_fonts ]; }}
    export GDK_BACKEND=broadway BROADWAY_DISPLAY=:7 GSK_RENDERER=cairo
    ${gtk4}/bin/gtk4-broadwayd :7 &
    broadway=$!
    for _ in $(seq 100); do
      for socket in "$XDG_RUNTIME_DIR"/broadway*.socket; do
        if [ -S "$socket" ]; then break 2; fi
      done
      sleep 0.1
    done

    test=${linux-ui.tests}/libexec/tsugumi-gtk/tsugumi_gtk_smoke_test
    if ! $host; then
      "$test"
      exit
    fi

    # A project of two Cards with nothing to build: a trigger and a Draft.
    project="$work/smoke"
    mkdir -p "$project/definitions/start" "$project/definitions/square"
    cat > "$project/tsugumi.json" <<'EOF'
    {"name": "Smoke", "pkgsPath": "tsugumi:default", "canvas": {"cards": [{"id": "start"}, {"id": "square"}],
     "connections": [{"from": {"card": "start", "port": "out"}, "to": {"card": "square", "port": "x"}}]}}
    EOF
    echo '{"template": "trigger"}' > "$project/definitions/start/_card.json"
    echo '{"template": "draft", "inputs": [{"name": "x"}]}' > "$project/definitions/square/_card.json"

    export TSUGUMI_HOST_STATE="$work/host-core" TSUGUMI_SOCKET_DIR="$work/s"
    mkdir -p "$TSUGUMI_SOCKET_DIR"
    export TSUGUMI_HOSTD=${core}/bin/tsugumi-hostd TSUGUMID=${core}/bin/tsugumid
    export TSUGUMI_WORKER=${core}/bin/tsugumi-worker TSUGUMI_SDK_DIR=${core}/share/tsugumi/sdk
    export TSUGUMI_REPO="$work/repo" TSUGUMI_WEB_ROOT="$work/no-web"
    mkdir -p "$TSUGUMI_REPO/demos"
    echo ${lib.fileContents ../VERSION} > "$TSUGUMI_REPO/VERSION"
    export TSUGUMI_GTK_TEST_SELF="$test" TSUGUMI_GTK_TEST_PROJECT="$project"
    if ! "$test"; then
      cat "$TSUGUMI_HOST_STATE"/host.log 2>/dev/null || true
      exit 1
    fi
  '';
  meta.platforms = linux-ui.meta.platforms;
}
