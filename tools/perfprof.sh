#!/usr/bin/env bash
# Profile the perfbench scene: run tools/perfbench.sh (SCENE and UNCAPPED pass through), wait for the mod's start
# mark plus DELAY seconds (default 5), `sample` build/mina4mac (or $MINA4MAC_BIN) for SECS seconds (default 20)
# every INTERVAL ms (default 5: at 1 ms, sampling ~40 threads slows the game by ~25%), let the benchmark finish,
# then summarize with tools/perfprof.py: time per bucket (guest code, dispatch, HLE, Lua, GL, swap, waiting...)
# and thread, wait sites, top guest functions. Everything goes to build/perfprof/<time>/ (sample.txt, report.txt,
# bench.txt with the run's fps and work_ms, which the sampling slows down a little). The perfbench mod must be
# installed (tools/perfbench.sh install mina4mac); keep the window visible, as for perfbench.
#   tools/perfprof.sh                 SCENE=heavy tools/perfprof.sh
#   uv run tools/perfprof.py build/perfprof/<time>/sample.txt     (re-summarize)
set -euo pipefail
cd "$(dirname "$0")/.."
DELAY=${DELAY:-5} SECS=${SECS:-20} INTERVAL=${INTERVAL:-5}
OUT=build/perfprof/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
FRAMES=build/game/perfbench_frames.txt
rm -f "$FRAMES"
tools/perfbench.sh run mina4mac "$OUT/bench.txt" >"$OUT/perfbench.out" 2>&1 &
bench=$!
for _ in $(seq 1 240); do
    grep -q '^mark start' "$FRAMES" 2>/dev/null && break
    kill -0 "$bench" 2>/dev/null || { cat "$OUT/perfbench.out"; exit 1; }
    sleep 0.5
done
grep -q '^mark start' "$FRAMES" || { echo "no start mark in $FRAMES"; exit 1; }
sleep "$DELAY"
BIN=${MINA4MAC_BIN:-build/mina4mac}
pid=$(pgrep -nf "^$BIN( |$)") || { echo "$BIN isn't running"; exit 1; }
echo "sampling pid $pid for $SECS s every $INTERVAL ms (scene ${SCENE:-flood}${UNCAPPED:+, uncapped}) -> $OUT"
sample "$pid" "$SECS" "$INTERVAL" -mayDie -file "$OUT/sample.txt" >/dev/null 2>&1
wait "$bench" || { cat "$OUT/perfbench.out"; echo "perfbench failed; the sample is still in $OUT"; }
sed -n 's/^PERFBENCH //p' "$OUT/bench.txt" 2>/dev/null | grep -E '^(frames|work_ms|cpu_ms|fps_per)' || true
uv run tools/perfprof.py "$OUT/sample.txt" | tee "$OUT/report.txt"
