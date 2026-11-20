# The Linux UI's headless check: its windows on GTK's Broadway display, and
# against a hostd the test starts, which opens a small project through an
# Agent's ShowRequest.
{
  lib,
  runCommand,
  gtk4,
  dejavu_fonts,
  makeFontsConf,
  linux-ui,
  core,
}:
runCommand "tsugumi-gtk-check"
  {
    meta.platforms = linux-ui.meta.platforms;
  }
  ''
    export HOME="$TMPDIR/home" XDG_RUNTIME_DIR="$TMPDIR/run"
    mkdir -p "$HOME" "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
    export FONTCONFIG_FILE=${makeFontsConf { fontDirectories = [ dejavu_fonts ]; }}
    export GDK_BACKEND=broadway BROADWAY_DISPLAY=:7 GSK_RENDERER=cairo
    ${gtk4}/bin/gtk4-broadwayd :7 &
    broadway=$!
    trap 'kill $broadway 2>/dev/null || true' EXIT
    for _ in $(seq 100); do
      compgen -G "$XDG_RUNTIME_DIR/broadway*.socket" > /dev/null && break
      sleep 0.1
    done

    # A project of two Cards with nothing to build: a trigger and a Draft.
    project="$TMPDIR/smoke"
    mkdir -p "$project/definitions/start" "$project/definitions/square"
    cat > "$project/tsugumi.json" <<'EOF'
    {"name": "Smoke", "canvas": {"cards": [{"id": "start"}, {"id": "square"}],
     "connections": [{"from": {"card": "start", "port": "out"}, "to": {"card": "square", "port": "x"}}]}}
    EOF
    echo '{"template": "trigger"}' > "$project/definitions/start/_card.json"
    echo '{"inputs": [{"name": "x"}]}' > "$project/definitions/square/_card.json"

    export TSUGUMI_HOST_STATE="$TMPDIR/host-core" TSUGUMI_SOCKET_DIR="$TMPDIR/s"
    mkdir -p "$TSUGUMI_SOCKET_DIR"
    export TSUGUMI_HOSTD=${core}/bin/tsugumi-hostd TSUGUMID=${core}/bin/tsugumid
    export TSUGUMI_WORKER=${core}/bin/tsugumi-worker TSUGUMI_SDK_DIR=${core}/share/tsugumi/sdk
    export TSUGUMI_REPO="$TMPDIR/repo" TSUGUMI_WEB_ROOT="$TMPDIR/no-web"
    mkdir -p "$TSUGUMI_REPO/demos"
    echo ${lib.fileContents ../VERSION} > "$TSUGUMI_REPO/VERSION"
    export TSUGUMI_GTK_TEST_SELF=${linux-ui.tests}/libexec/tsugumi-gtk/tsugumi_gtk_smoke_test
    export TSUGUMI_GTK_TEST_PROJECT="$project"
    "$TSUGUMI_GTK_TEST_SELF" || { cat "$TSUGUMI_HOST_STATE"/host.log 2>/dev/null || true; exit 1; }
    touch $out
  ''
