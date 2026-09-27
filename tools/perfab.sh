#!/usr/bin/env bash
# A/B benchmark of two mina4mac binaries: n interleaved pairs of tools/perfbench.sh runs (ABBA order, so a slow
# drift of the machine hits both sides equally), then the median and spread (min-max) of fps, work_ms and cpu_ms
# per side and B's change against A. SCENE and UNCAPPED pass through to perfbench.sh; the perfbench mod must be
# installed (tools/perfbench.sh install mina4mac). Each run's summary is kept in build/perfab/<time>/.
#   cp build/mina4mac build/mina4mac.A; <change, rebuild>; tools/perfab.sh build/mina4mac.A build/mina4mac 3
# A run that stalls or doesn't finish is retried once (a stalled run is invalid, not a finding); a second failure
# stops the comparison.
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -ge 2 ] || { sed -n '2,8p' "$0"; exit 2; }
A=$1 B=$2 N=${3:-3}
for b in "$A" "$B"; do [ -x "$b" ] || { echo "not an executable: $b"; exit 1; }; done
OUT=build/perfab/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
echo "A=$A B=$B n=$N scene=${SCENE:-flood}${UNCAPPED:+ uncapped} -> $OUT"

run() {  # run <side> <binary> <i>
    local f="$OUT/$1_$3.txt"
    for try in 1 2; do
        if MINA4MAC_BIN=$2 tools/perfbench.sh run mina4mac "$f" >/dev/null 2>&1 && grep -q 'work_ms' "$f"; then
            # a frame over 1 s means the window stalled (see PLAN2): invalid
            if awk '/^PERFBENCH frame_ms/{exit !($10 < 1000)}' "$f"; then
                echo "$1 $3: $(awk '/ fps /{printf "%s fps", $7} / work_ms /{printf ", work %s ms", $4} / cpu_ms /{printf ", cpu %s ms", $4}' "$f")"
                return
            fi
            echo "$1 $3: stalled (max frame $(awk '/^PERFBENCH frame_ms/{print $10}' "$f") ms), retrying"
        else
            echo "$1 $3: failed (try $try)"
        fi
    done
    echo "giving up"; exit 1
}

for i in $(seq 1 "$N"); do
    if [ $((i % 2)) = 1 ]; then run A "$A" "$i"; run B "$B" "$i"; else run B "$B" "$i"; run A "$A" "$i"; fi
done

# field <side> <line key> <awk field>: that value from each of the side's runs, one per line
field() { cat "$OUT/$1"_*.txt | awk -v k="$2" -v f="$3" '$2 == k {print $f}'; }
stats() { sort -n | awk '{v[NR]=$1} END{printf "%.2f %.2f %.2f", (NR%2 ? v[(NR+1)/2] : (v[NR/2]+v[NR/2+1])/2), v[1], v[NR]}'; }
printf "%-10s %24s %24s %8s\n" "" "A median (min-max)" "B median (min-max)" "B vs A"
for m in "fps frames 7" "work_ms work_ms 4" "work_p95 work_ms 6" "cpu_ms cpu_ms 4" "cpu_p95 cpu_ms 6"; do
    set -- $m
    read -r am alo ahi <<<"$(field A "$2" "$3" | stats)"
    read -r bm blo bhi <<<"$(field B "$2" "$3" | stats)"
    printf "%-10s %8s (%6s-%6s) %8s (%6s-%6s) %+7.1f%%\n" "$1" "$am" "$alo" "$ahi" "$bm" "$blo" "$bhi" \
        "$(awk -v a="$am" -v b="$bm" 'BEGIN{print a ? (b - a) / a * 100 : 0}')"
done | tee "$OUT/summary.txt"
