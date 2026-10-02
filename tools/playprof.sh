#!/usr/bin/env bash
# Profile the running game while it's slow, without stopping it: `sample` the newest mina4mac process (the
# packaged Noita.app's, or build/mina4mac) for SECS seconds (default 20) every INTERVAL ms (default 5, as
# tools/perfprof.sh), then summarize it with tools/perfprof.py. Also keeps the [fps] lines logged meanwhile (fps,
# main work/cpu, telemetry). Everything goes to build/playprof/<time>/ (sample.txt, report.txt, fps.txt).
# perfprof.py maps symbols with nm over build/gen_all/*.o, so the app must come from the current build (as
# tools/package_app.sh makes it); a stale build still gets its guest functions right, runtime buckets may be off.
#   tools/playprof.sh            SECS=40 tools/playprof.sh
set -euo pipefail
cd "$(dirname "$0")/.."
SECS=${SECS:-20} INTERVAL=${INTERVAL:-5}
LOG="$HOME/Library/Logs/mina4mac.log"
pid=$(pgrep -n -x mina4mac) || { echo "mina4mac isn't running"; exit 1; }
OUT=build/playprof/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
before=$(wc -l <"$LOG" 2>/dev/null || echo 0)
echo "sampling pid $pid ($(ps -o comm= -p "$pid")) for $SECS s every $INTERVAL ms -> $OUT"
sample "$pid" "$SECS" "$INTERVAL" -mayDie -file "$OUT/sample.txt" >/dev/null 2>&1
[ -f "$LOG" ] && tail -n +"$((before + 1))" "$LOG" | grep '^\[fps\]' >"$OUT/fps.txt" || true
[ -s "$OUT/fps.txt" ] && { echo "fps while sampling (the sampling slows the game a little):"; cat "$OUT/fps.txt"; }
uv run tools/perfprof.py "$OUT/sample.txt" | tee "$OUT/report.txt"
