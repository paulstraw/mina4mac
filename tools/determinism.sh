#!/usr/bin/env bash
# Determinism check between builds: tools/seedprint is a mod that pins WORLD_SEED and logs a fingerprint of the
# start area to <game dir>/seedprint.txt. Install it into each game, start a New Game in each, leave the controls
# alone for about 15 s after spawning (the last line is "SEEDPRINT done"), quit, then diff.
# The mod needs the unrestricted Lua API (release builds drop print() output), so install turns the mod sandbox off
# (mods_sandbox_enabled="0") and remove turns it back on.
#   tools/determinism.sh install <game dir> <Nolla_Games_Noita save dir>    copy + enable the mod (game not running)
#   tools/determinism.sh remove  <game dir> <Nolla_Games_Noita save dir>    disable + delete it again
#   tools/determinism.sh diff [a b]      compare two seedprint.txt files (default: mina4mac's and the Wine build's);
#                                        FULL=1 also prints the first 60 lines of the raw diff
#   tools/determinism.sh run [binary]    unattended, mina4mac only: pause the perfbench mod, install seedprint, start a
#                                        fresh run with -gamemode 0 (no menu), wait for "SEEDPRINT done", stop the game,
#                                        restore the mods, then diff against the Wine build (default build/mina4mac)
# Defaults: mina4mac is build/game with ~/Library/Application Support/mina4mac/.../Nolla_Games_Noita; the Wine build
# (tools/determinism.sh install wine) is the Sikarugir wrapper's GOG install and save.
set -euo pipefail
cd "$(dirname "$0")/.."

WINE_PREFIX="$HOME/Applications/Noita Sikarugir.app/Contents/SharedSupport/prefix/drive_c"
dirs() {  # dirs <name|game dir> [save dir]
    case "$1" in
    mina4mac) GAME=build/game SAVE="$HOME/Library/Application Support/mina4mac/AppData/LocalLow/Nolla_Games_Noita" ;;
    wine) GAME="$WINE_PREFIX/GOG Games/Noita" SAVE="$WINE_PREFIX/users/Sikarugir/AppData/LocalLow/Nolla_Games_Noita" ;;
    *) GAME=$1 SAVE=$2 ;;
    esac
}

set_mod() {  # set_mod <0|1>: seedprint's entry in save00/mod_config.xml
    local cfg="$SAVE/save00/mod_config.xml"
    [ -f "$cfg" ] || { echo "no $cfg (run the game once first)"; exit 1; }
    if grep -q 'name="seedprint"' "$cfg"; then
        sed -i '' -E "s/<Mod enabled=\"[01]\" name=\"seedprint\"/<Mod enabled=\"$1\" name=\"seedprint\"/" "$cfg"
    elif [ "$1" = 1 ]; then
        sed -i '' 's#</Mods>#  <Mod enabled="1" name="seedprint" settings_fold_open="0" workshop_item_id="0" >\
  </Mod>\
</Mods>#' "$cfg"
    fi
}

case "${1:-}" in
install)
    dirs "${2:?game dir}" "${3:-}"
    rsync -a --delete tools/seedprint/ "$GAME/mods/seedprint/"
    set_mod 1
    sed -i '' -E 's/mods_disclaimer_accepted="0"/mods_disclaimer_accepted="1"/; s/mods_sandbox_enabled="1"/mods_sandbox_enabled="0"/' \
        "$SAVE/save_shared/config.xml"
    rm -f "$GAME/seedprint.txt"
    echo "installed and enabled seedprint in $GAME (save $SAVE)" ;;
remove)
    dirs "${2:?game dir}" "${3:-}"
    set_mod 0
    sed -i '' -E 's/mods_sandbox_enabled="0"/mods_sandbox_enabled="1"/' "$SAVE/save_shared/config.xml"
    rm -rf "$GAME/mods/seedprint"
    echo "removed seedprint from $GAME" ;;
diff)
    dirs mina4mac; A=${2:-$GAME/seedprint.txt}
    dirs wine; B=${3:-$GAME/seedprint.txt}
    a=$(mktemp) b=$(mktemp)
    sed $'s/\r$//; s/^SEEDPRINT //' "$A" >"$a"
    sed $'s/\r$//; s/^SEEDPRINT //' "$B" >"$b"
    echo "$(wc -l <"$a") vs $(wc -l <"$b") lines"
    # seed, RNG and libm don't depend on the world and must match (libm: 2 known last-bit values vs Wine). The
    # f1/f60/f600 snapshots move with thread timing: two runs of one binary differ in ~50 lines at f60 and f600
    # (2026-09-28), so compare those counts with a same-binary pair, not with zero.
    for k in seed procedural random libm; do
        if diff <(grep "^$k " "$a") <(grep "^$k " "$b") >/dev/null; then echo "$k: identical"; else echo "$k: DIFFERS"; fi
    done
    for k in f1 f60 f600; do
        echo "$k: $(diff <(grep "^$k " "$a") <(grep "^$k " "$b") | grep -c '^<') of $(grep -c "^$k " "$a") lines differ"
    done
    [ -n "${FULL:-}" ] && diff "$a" "$b" | head -60 || true ;;
run)
    BIN=${2:-build/mina4mac}
    dirs mina4mac
    CFG="$SAVE/save00/mod_config.xml"
    pb=0; grep -q '<Mod enabled="1" name="perfbench"' "$CFG" && pb=1
    sb=$(grep -oE 'mods_sandbox_enabled="[01]"' "$SAVE/save_shared/config.xml")
    sed -i '' 's/<Mod enabled="1" name="perfbench"/<Mod enabled="0" name="perfbench"/' "$CFG"
    "$0" install mina4mac >/dev/null
    rm -rf "$SAVE/save00/world" "$SAVE/save00/player.xml" "$SAVE/save00/world_state.xml"  # a new run, as a New Game
    caffeinate -d -u "$BIN" -no_logo_splashes -gamemode 0 >build/determinism_run.log 2>&1 &
    pid=$!
    # the game writes its config on exit, so the mods are restored only after it has stopped
    trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null; "$0" remove mina4mac >/dev/null
          sed -i "" -E "s/mods_sandbox_enabled=\"[01]\"/$sb/" "$SAVE/save_shared/config.xml"
          [ $pb = 1 ] && sed -i "" "s/<Mod enabled=\"0\" name=\"perfbench\"/<Mod enabled=\"1\" name=\"perfbench\"/" "$CFG"' EXIT
    for _ in $(seq 1 240); do
        grep -q "SEEDPRINT done" "$GAME/seedprint.txt" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.5
    done
    grep -q "SEEDPRINT done" "$GAME/seedprint.txt" 2>/dev/null || { echo "no SEEDPRINT done (log: build/determinism_run.log)"; exit 1; }
    "$0" diff ;;
*) sed -n '2,15p' "$0"; exit 2 ;;
esac
