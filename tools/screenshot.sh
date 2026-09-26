#!/usr/bin/env bash
# Run the launcher, wait for its "Noita" window and save a screenshot of that window (screencapture -l).
# The launcher is killed afterwards if it's still running.
# Usage: tools/screenshot.sh [out.png] [delay seconds after the window appears]   (default build/window.png, 0.3)
# Needs the display awake: with it asleep, SDL_GL_SwapWindow blocks on vsync (caffeinate -u wakes it).
set -uo pipefail
cd "$(dirname "$0")/.."
OUT=${1:-build/window.png}
DELAY=${2:-0.3}
clang -O2 tools/winlist.c -framework CoreGraphics -framework CoreFoundation -o build/winlist || exit 1
build/noitamac >build/screenshot.log 2>&1 &
pid=$!
for _ in $(seq 1 1000); do
    win=$(build/winlist "$pid" Noita | head -1)
    [ -n "$win" ] || { kill -0 "$pid" 2>/dev/null && sleep 0.02 && continue; }
    break
done
if [ -z "$win" ]; then echo "no Noita window (launcher log: build/screenshot.log)"; exit 1; fi
sleep "$DELAY"
screencapture -x -o -l "${win%% *}" "$OUT" && echo "window ${win}: saved $OUT"
kill "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
exit 0
