#!/usr/bin/env bash
# Profile-guided build of build/mina4mac (IR PGO). The profile comes from the user's own game, so it lives in
# build/pgo/ and is never committed.
#   tools/pgo.sh [all]   gen, train, merge, order, use
#   tools/pgo.sh gen     instrumented build (tools/build_all.py --pgo gen); clears build/pgo/raw/
#   tools/pgo.sh train   run the instrumented build through tools/perfbench.sh: SCENES (default "flood heavy"),
#                        RUNS (default 1) times each; the perfbench mod must be installed
#   tools/pgo.sh merge   llvm-profdata merge build/pgo/raw/ -> build/pgo/mina4mac.profdata
#   tools/pgo.sh order   build/pgo/mina4mac.order from the profile (tools/pgo_order.py), hottest functions first
#   tools/pgo.sh use     optimized build with that profile and order file (tools/build_all.py --pgo use --order)
# The instrumented game runs slower; perfbench still finishes. Leave the window visible, as for any perfbench run.
set -euo pipefail
cd "$(dirname "$0")/.."
PGO=build/pgo

gen() {
    rm -rf "$PGO/raw"
    mkdir -p "$PGO/raw"
    uv run tools/build_all.py --pgo gen
}
train() {
    for s in ${SCENES:-flood heavy}; do
        for i in $(seq 1 "${RUNS:-1}"); do
            echo "training: $s $i"
            SCENE=$s tools/perfbench.sh run mina4mac "$PGO/train_${s}_$i.txt" | grep -E ' fps |work_ms' || true
        done
    done
    ls -l "$PGO"/raw/*.profraw
}
merge() {
    "$(clang -print-prog-name=llvm-profdata)" merge -o "$PGO/mina4mac.profdata" "$PGO"/raw/*.profraw
    ls -l "$PGO/mina4mac.profdata"
}
order() { uv run tools/pgo_order.py "$PGO/mina4mac.profdata" "$PGO/mina4mac.order"; }
use() { uv run tools/build_all.py --pgo use --order; }

case "${1:-all}" in
all) gen; train; merge; order; use ;;
gen | train | merge | order | use) "$1" ;;
*) sed -n '2,12p' "$0"; exit 2 ;;
esac
