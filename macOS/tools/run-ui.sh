#!/bin/bash
# Runs the app with a UI script (UIScript.swift) and saves a screenshot of the
# named window at every "capture <name> [window title]" step.
#
#   macOS/tools/run-ui.sh <Tsugumi.app> <script> <out dir> [project folder]
set -u
app="$1"; script="$2"; out="$3"; project="${4:-}"
mkdir -p "$out"
log="$out/ui.log"; : > "$log"
TSUGUMI_UI_SCRIPT="$script" TSUGUMI_UI_LOG="$log" "$app/Contents/MacOS/Tsugumi" $project > "$out/app.out" 2>&1 &
pid=$!
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
kill $pid 2>/dev/null
grep -v '^step' "$log"
