#!/bin/bash
# Runs the app with a UI script (UIScript.swift) and saves a screenshot of the
# named window at every "capture <name> [window title]" step.
#
#   ui/macos/tools/run-ui.sh <Tsugumi.app> <script> <out dir> [project folder] [-<setting> <value> …]
#
# The app opens behind what you are doing (`open -g`), and the pictures are
# taken all the same, with the window in its inactive colors.
#
# Settings after the project folder are passed as arguments, which UserDefaults
# reads before the saved ones and never saves. The Agent's settings (provider,
# model) live in the host service: set them from the script with
# `setting <key> <json>`, which saves them.
# TSUGUMI_UI_TRUSTED_FOLDER=/absolute/folder registers that folder before opening
# projects. Like `setting`, this saves a host setting (remove it in Settings).
# TSUGUMI_HOST_STATE can select a separate host state for isolated trials.
# TSUGUMI_UI_LANGUAGE=en (or ja) does the same for the app's language.
set -u
absolute() { (cd "$(dirname "$1")" && echo "$(pwd -P)/$(basename "$1")"); }
mkdir -p "$3"
app="$(absolute "$1")"; script="$(absolute "$2")"; out="$(cd "$3" && pwd -P)"
project=""; [ -z "${4:-}" ] || project="$(absolute "$4")"
settings=("${@:5}")
log="$out/ui.log"; : > "$log"
# `open` starts the app from launchd: it inherits nothing from this shell, and
# its working directory is /.
options=(-g -n -o "$out/app.out" --stderr "$out/app.out")
options+=(--env "TSUGUMI_UI_SCRIPT=$script" --env "TSUGUMI_UI_LOG=$log")
for name in "${!TSUGUMI_@}"; do
  case "$name" in TSUGUMI_UI_SCRIPT|TSUGUMI_UI_LOG|TSUGUMI_UI_LANGUAGE) continue ;; esac
  options+=(--env "$name=${!name}")
done
args=()
[ -z "$project" ] || args+=("$project")
args+=(${settings[@]+"${settings[@]}"})
if [ -n "${TSUGUMI_UI_LANGUAGE:-}" ]; then
  # The argument domain is read before the saved setting, and the app only
  # writes AppleLanguages when it differs from the language.
  args+=(-app.language "$TSUGUMI_UI_LANGUAGE" -AppleLanguages "($TSUGUMI_UI_LANGUAGE)")
fi
open "${options[@]}" "$app" ${args[@]+--args "${args[@]}"}
pid=""
for _ in $(seq 1 50); do
  pid="$(pgrep -n -f "$app/Contents/MacOS/Tsugumi")" && break
  sleep 0.1
done
[ -n "$pid" ] || { echo "$app did not start" >&2; exit 1; }
seen=0
# TSUGUMI_UI_SECONDS: how long the script may run (default 600).
for _ in $(seq 1 $(( ${TSUGUMI_UI_SECONDS:-600} * 5 ))); do
  kill -0 $pid 2>/dev/null || break
  n=$(wc -l < "$log")
  if [ "$n" -gt "$seen" ]; then
    sed -n "$((seen + 1)),${n}p" "$log" | while read -r kind name win rest; do
      if [ "$kind" = capture ] && [ "$win" != "-1" ]; then
        sleep 0.6
        screencapture -l "$win" -o -x "$out/$name.png" && echo "captured $name"
      fi
    done
    seen=$n
    grep -q '^done' "$log" && break
  fi
  sleep 0.2
done
sleep 1
# TERM ends the app the way Quit does (without its questions): the app stops the
# host service and the VM in order, then exits. Wait for that.
kill $pid 2>/dev/null
for _ in $(seq 1 100); do
  kill -0 $pid 2>/dev/null || break
  sleep 0.1
done
grep -v '^step' "$log" || exit $?
! grep -q '^trust-error ' "$log"
