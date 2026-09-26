#!/usr/bin/env bash
# Performance benchmark, the same scene in noitamac and in the Wine build: tools/perfbench is a mod that pins
# WORLD_SEED, floods the start area with water, oil and lava 1 s after spawning, then logs the real time of 1800
# frames and writes a summary (fps, frame-time percentiles, fps per 300 frames) to <game dir>/perfbench.txt.
# The game is started with `-no_logo_splashes -gamemode 0`, which skips the menu and starts a new run (this
# replaces the run in progress in save00). Like tools/determinism.sh, install turns the mod sandbox off (the mod
# writes a file) and remove turns it back on.
#   tools/perfbench.sh install noitamac|wine     copy + enable the mod (game not running)
#   tools/perfbench.sh run noitamac|wine [out]   run the benchmark once, print the summary (and copy it to out);
#                                                with UNCAPPED=1, vsync is off and the frame limit 1000 for the run
#   tools/perfbench.sh remove noitamac|wine      disable + delete it again
# noitamac runs build/noitamac (or $NOITAMAC_BIN, e.g. a copy for A/B runs) with extra arguments in NOITAMAC_ARGS.
# The Wine build runs the Sikarugir wrapper (~/Applications/Noita Sikarugir.app), unchanged apart from its Program
# Flags during the run. Keep the game window visible and the Mac otherwise idle: an occluded window can stall a
# frame for many seconds, and results vary by about ±7% between runs, so compare medians of interleaved runs.
set -euo pipefail
cd "$(dirname "$0")/.."

APP="$HOME/Applications/Noita Sikarugir.app"
WINE_PREFIX="$APP/Contents/SharedSupport/prefix"
ARGS=(-no_logo_splashes -gamemode 0)
dirs() {
    case "$1" in
    noitamac) GAME=build/game SAVE="$HOME/Library/Application Support/noitamac/AppData/LocalLow/Nolla_Games_Noita" ;;
    wine) GAME="$WINE_PREFIX/drive_c/GOG Games/Noita"
          SAVE="$WINE_PREFIX/drive_c/users/Sikarugir/AppData/LocalLow/Nolla_Games_Noita" ;;
    *) echo "noitamac or wine"; exit 2 ;;
    esac
}

set_mod() {  # set_mod <0|1>: perfbench's entry in save00/mod_config.xml
    local cfg="$SAVE/save00/mod_config.xml"
    [ -f "$cfg" ] || { echo "no $cfg (run the game once first)"; exit 1; }
    if grep -q 'name="perfbench"' "$cfg"; then
        sed -i '' -E "s/<Mod enabled=\"[01]\" name=\"perfbench\"/<Mod enabled=\"$1\" name=\"perfbench\"/" "$cfg"
    elif [ "$1" = 1 ]; then
        sed -i '' 's#</Mods>#  <Mod enabled="1" name="perfbench" settings_fold_open="0" workshop_item_id="0" >\
  </Mod>\
</Mods>#' "$cfg"
    fi
}

case "${1:-}" in
install)
    dirs "${2:-}"
    rsync -a --delete tools/perfbench/ "$GAME/mods/perfbench/"
    set_mod 1
    sed -i '' -E 's/mods_disclaimer_accepted="0"/mods_disclaimer_accepted="1"/; s/mods_sandbox_enabled="1"/mods_sandbox_enabled="0"/' \
        "$SAVE/save_shared/config.xml"
    echo "installed and enabled perfbench in $GAME (save $SAVE)" ;;
remove)
    dirs "${2:-}"
    set_mod 0
    sed -i '' -E 's/mods_sandbox_enabled="0"/mods_sandbox_enabled="1"/' "$SAVE/save_shared/config.xml"
    rm -rf "$GAME/mods/perfbench"
    echo "removed perfbench from $GAME" ;;
run)
    dirs "${2:-}"
    [ -d "$GAME/mods/perfbench" ] || { echo "run tools/perfbench.sh install $2 first"; exit 1; }
    rm -f "$GAME/perfbench.txt"
    log=build/perfbench_$2.log
    CFG="$SAVE/save_shared/config.xml"
    cleanup() { :; }
    if [ -n "${UNCAPPED:-}" ]; then  # no vsync, no frame limit: frame times show CPU headroom above 60 fps
        v=$(grep -oE ' vsync="[0-9]+"' "$CFG") f=$(grep -oE ' framerate="[0-9]+"' "$CFG")
        sed -i '' -E 's/ vsync="[0-9]+"/ vsync="0"/; s/ framerate="[0-9]+"/ framerate="1000"/' "$CFG"
        # the game writes its config on exit, so this runs after it has stopped
        cleanup() { sed -i '' -E "s/ vsync=\"[0-9]+\"/$v/; s/ framerate=\"[0-9]+\"/$f/" "$CFG"; }
    fi
    trap 'cleanup' EXIT
    if [ "$2" = noitamac ]; then
        # shellcheck disable=SC2086
        caffeinate -d -u "${NOITAMAC_BIN:-build/noitamac}" "${ARGS[@]}" ${NOITAMAC_ARGS:-} >"$log" 2>&1 &
        pid=$!
        stop() { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true; }
    else
        # Direct `wine noita.exe` fails here (wineboot won't start), so launch through the wrapper with its
        # "Program Flags" set for this run only (restored on exit), and stop it with wineserver -k.
        W="$APP/Contents/SharedSupport/wine"
        plutil -replace "Program Flags" -string "${ARGS[*]}" "$APP/Contents/Info.plist"
        trap 'plutil -replace "Program Flags" -string "" "$APP/Contents/Info.plist"; cleanup' EXIT
        open "$APP"
        pid=
        # the game process itself ("C:\GOG Games\Noita\noita.exe ..."), not wine's short-lived helper processes
        for _ in $(seq 1 60); do pid=$(pgrep -f '^C:.*noita\.exe' | head -1); [ -n "$pid" ] && break; sleep 1; done
        [ -n "$pid" ] || { echo "the wrapper didn't start noita.exe"; exit 1; }
        caffeinate -d -u -w "$pid" &
        stop() {
            WINEPREFIX="$WINE_PREFIX" DYLD_FALLBACK_LIBRARY_PATH="$W/lib:$APP/Contents/Frameworks:/usr/lib" "$W/bin/wineserver" -k || true
            sleep 2; pkill -f "$APP/Contents/MacOS/Sikarugir" || true
        }
    fi
    for _ in $(seq 1 600); do
        grep -q "PERFBENCH done" "$GAME/perfbench.txt" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.5
    done
    stop
    [ -f "$GAME/perfbench.txt" ] || { echo "no perfbench.txt (log: $log)"; exit 1; }
    tr -d '\r' <"$GAME/perfbench.txt" | sed 's/^PERFBENCH //'
    [ -z "${3:-}" ] || tr -d '\r' <"$GAME/perfbench.txt" >"$3"
    grep -q "PERFBENCH done" "$GAME/perfbench.txt" || { echo "(incomplete; log: $log)"; exit 1; } ;;
*) sed -n '2,12p' "$0"; exit 2 ;;
esac
