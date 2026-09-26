#!/usr/bin/env bash
# Full verification sequence: survey, build_all, difftest (default and --x87), x87regtest, atomictest, importtest,
# hosttest, envtest, loadtest, undnametest, sdltest, luatest, gltest, then a launcher run (informational: prints where build/noitamac stops).
# Exits non-zero if the lifted count drops below tools/lifted_baseline.txt or any
# difftest step reports fail/native_err, or x87regtest/atomictest/importtest/hosttest/envtest/loadtest/undnametest/sdltest/luatest/gltest fails. A higher lifted count raises the baseline.
# Usage: tools/check.sh [seed]   (default: random; printed so a run can be repeated)
set -uo pipefail
cd "$(dirname "$0")/.."

SEED=${1:-$RANDOM}
LOG=build/check
mkdir -p "$LOG"
BASELINE_FILE=tools/lifted_baseline.txt
status=0

step() {  # step <name> <cmd...>: run, log to $LOG/<name>.log, abort on non-zero exit
    local name=$1; shift
    "$@" >"$LOG/$name.log" 2>&1
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "$name: FAILED (exit $rc), see $LOG/$name.log"
        exit 1
    fi
}

step survey uv run tools/survey.py
lifted=$(sed -nE 's/^functions lifted ([0-9,]+)\/([0-9,]+).*/\1/p' "$LOG/survey.log" | tr -d ,)
total=$(sed -nE 's/^functions lifted ([0-9,]+)\/([0-9,]+).*/\2/p' "$LOG/survey.log" | tr -d ,)
baseline=$(cat "$BASELINE_FILE")
if [ -z "$lifted" ]; then
    echo "survey: could not parse lifted count"; exit 1
elif [ "$lifted" -lt "$baseline" ]; then
    echo "survey: FAIL lifted $lifted/$total, below baseline $baseline"; status=1
else
    [ "$lifted" -gt "$baseline" ] && echo "$lifted" >"$BASELINE_FILE"
    echo "survey: ok lifted $lifted/$total (baseline $baseline)"
fi

step build_all uv run tools/build_all.py
echo "build_all: ok $(head -1 "$LOG/build_all.log")"

for mode in default x87; do
    flag=""; [ "$mode" = x87 ] && flag=--x87
    step "difftest_$mode" uv run tools/difftest.py --all --funcs 1500 --trials 3 --seed "$SEED" $flag
    stats=$(grep -E "^\{'pass_'" "$LOG/difftest_$mode.log")
    fail=$(sed -nE "s/.*'fail': ([0-9]+).*/\1/p" <<<"$stats")
    nerr=$(sed -nE "s/.*'native_err': ([0-9]+).*/\1/p" <<<"$stats")
    if [ -z "$stats" ] || [ "$fail" != 0 ] || [ "$nerr" != 0 ]; then
        echo "difftest $mode (seed $SEED): FAIL ${stats:-no stats}, see $LOG/difftest_$mode.log"; status=1
    else
        echo "difftest $mode (seed $SEED): ok $stats"
    fi
done

if uv run tools/insntest.py --x87reg --seed "$SEED" >"$LOG/x87regtest.log" 2>&1; then
    echo "x87regtest: ok $(tail -1 "$LOG/x87regtest.log")"
else
    echo "x87regtest: FAIL, see $LOG/x87regtest.log"; status=1
fi

if uv run tools/atomictest.py >"$LOG/atomictest.log" 2>&1; then
    echo "atomictest: ok $(tail -1 "$LOG/atomictest.log")"
else
    echo "atomictest: FAIL, see $LOG/atomictest.log"; status=1
fi

if uv run tools/importtest.py >"$LOG/importtest.log" 2>&1; then
    echo "importtest: ok $(grep -c ' ok ' "$LOG/importtest.log") checks"
else
    echo "importtest: FAIL, see $LOG/importtest.log"; status=1
fi

if clang -O2 -ffp-contract=off -fno-strict-aliasing -Wall -Wextra -Werror -I runtime runtime/host_test.c runtime/rt.c \
        -o build/hosttest >"$LOG/hosttest.log" 2>&1 && build/hosttest >>"$LOG/hosttest.log" 2>&1; then
    echo "hosttest: ok $(grep -c ' ok ' "$LOG/hosttest.log") checks"
else
    echo "hosttest: FAIL, see $LOG/hosttest.log"; status=1
fi

# envtest needs build/noita/image.bin, which difftest writes, and build/gen_all/gl_gen.c (tools/gen_gl.py via build_all).
# The launcher's runtime files (build_all.py LAUNCHER) minus main.c.
HLE=$(sed -nE 's/^LAUNCHER = \((.*)\).*/\1/p' tools/build_all.py | tr -d '",' | tr ' ' '\n' | grep -v '^main.c$' | sed 's|^|runtime/|')
if clang -O2 -ffp-contract=off -fno-strict-aliasing -Wall -Wextra -Werror -I runtime runtime/env_test.c runtime/rt.c -framework OpenGL \
        $HLE build/gen_all/gl_gen.c -o build/envtest >"$LOG/envtest.log" 2>&1 \
        && build/envtest build/noita/image.bin "${NOITA_DIR:-build/game}/msvcp120.dll" >>"$LOG/envtest.log" 2>&1; then
    echo "envtest: ok $(grep -c ' ok ' "$LOG/envtest.log") checks"
else
    echo "envtest: FAIL, see $LOG/envtest.log"; status=1
fi

if uv run tools/loadtest.py >"$LOG/loadtest.log" 2>&1; then
    echo "loadtest: ok $(grep -c ' 0 differ' "$LOG/loadtest.log") modules"
else
    echo "loadtest: FAIL, see $LOG/loadtest.log"; status=1
fi

if uv run tools/undnametest.py >"$LOG/undnametest.log" 2>&1; then
    echo "undnametest: ok $(tail -1 "$LOG/undnametest.log")"
else
    echo "undnametest: FAIL, see $LOG/undnametest.log"; status=1
fi

# sdltest needs build/gen_all/sdl2_gen.c and sdl2_layout.h, which build_all generates (tools/gen_sdl.py).
if clang -O2 -ffp-contract=off -fno-strict-aliasing -Wall -Wextra -Werror -I runtime -I build/gen_all $(sdl2-config --cflags) \
        runtime/sdl_test.c runtime/rt.c runtime/heap.c runtime/hle.c runtime/sdl2.c runtime/sdl2_stdlib.c \
        build/gen_all/sdl2_gen.c $(sdl2-config --libs) -o build/sdltest >"$LOG/sdltest.log" 2>&1 \
        && build/sdltest >>"$LOG/sdltest.log" 2>&1; then
    echo "sdltest: ok $(grep -c ' ok ' "$LOG/sdltest.log") checks"
else
    echo "sdltest: FAIL, see $LOG/sdltest.log"; status=1
fi

# luatest needs build/luajit/src/libluajit.a, which build_all builds from third_party/luajit.
if clang -O2 -ffp-contract=off -fno-strict-aliasing -Wall -Wextra -Werror -fexceptions -I runtime -I third_party/luajit/src \
        runtime/lua_test.c runtime/lua51.c runtime/rt.c runtime/proc.c runtime/heap.c runtime/hle.c build/luajit/src/libluajit.a \
        -o build/luatest >"$LOG/luatest.log" 2>&1 && build/luatest >>"$LOG/luatest.log" 2>&1; then
    echo "luatest: ok $(grep -c ' ok ' "$LOG/luatest.log") checks; $(grep '^jit: [0-9]' "$LOG/luatest.log")"
else
    echo "luatest: FAIL, see $LOG/luatest.log"; status=1
fi

# gltest needs build/gen_all/gl_gen.c; it draws into a framebuffer object of a hidden window's GL context.
if clang -O2 -ffp-contract=off -fno-strict-aliasing -Wall -Wextra -Werror -I runtime $(sdl2-config --cflags) \
        runtime/gl_test.c runtime/opengl32.c build/gen_all/gl_gen.c runtime/rt.c runtime/heap.c runtime/hle.c \
        $(sdl2-config --libs) -framework OpenGL -o build/gltest >"$LOG/gltest.log" 2>&1 && build/gltest >>"$LOG/gltest.log" 2>&1; then
    echo "gltest: ok $(grep -c ' ok ' "$LOG/gltest.log") checks"
else
    echo "gltest: FAIL, see $LOG/gltest.log"; status=1
fi

# The launcher opens a window; with the display asleep it blocks in SDL_GL_SwapWindow (vsync), hence the timeout.
# The scripted clicks pick New Game, then the first game mode (window points of the default 1280x720 window).
NOITAMAC_CLICKS="40:639,352;45:445,250" timeout 300 build/noitamac >"$LOG/launcher.log" 2>&1
rc=$?
stop=$(grep -m1 -E '^(unimplemented|unimpl|no function|guest)' "$LOG/launcher.log" || tail -1 "$LOG/launcher.log")
echo "launcher: exit $rc, $stop"

exit $status
