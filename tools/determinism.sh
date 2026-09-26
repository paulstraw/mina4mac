#!/usr/bin/env bash
# Determinism check between builds: tools/seedprint is a mod that pins WORLD_SEED and logs a fingerprint of the
# start area to <game dir>/seedprint.txt. Install it into each game, start a New Game in each, leave the controls
# alone for about 15 s after spawning (the last line is "SEEDPRINT done"), quit, then diff.
# The mod needs the unrestricted Lua API (release builds drop print() output), so install turns the mod sandbox off
# (mods_sandbox_enabled="0") and remove turns it back on.
#   tools/determinism.sh install <game dir> <Nolla_Games_Noita save dir>    copy + enable the mod (game not running)
#   tools/determinism.sh remove  <game dir> <Nolla_Games_Noita save dir>    disable + delete it again
#   tools/determinism.sh diff [a b]      compare two seedprint.txt files (default: noitamac's and the Wine build's)
# Defaults: noitamac is build/game with ~/Library/Application Support/noitamac/.../Nolla_Games_Noita; the Wine build
# (tools/determinism.sh install wine) is the Sikarugir wrapper's GOG install and save.
set -euo pipefail
cd "$(dirname "$0")/.."

WINE_PREFIX="$HOME/Applications/Noita Sikarugir.app/Contents/SharedSupport/prefix/drive_c"
dirs() {  # dirs <name|game dir> [save dir]
    case "$1" in
    noitamac) GAME=build/game SAVE="$HOME/Library/Application Support/noitamac/AppData/LocalLow/Nolla_Games_Noita" ;;
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
    dirs noitamac; A=${2:-$GAME/seedprint.txt}
    dirs wine; B=${3:-$GAME/seedprint.txt}
    a=$(mktemp) b=$(mktemp)
    sed $'s/\r$//; s/^SEEDPRINT //' "$A" >"$a"
    sed $'s/\r$//; s/^SEEDPRINT //' "$B" >"$b"
    echo "$(wc -l <"$a") vs $(wc -l <"$b") lines"
    if diff "$a" "$b" >/dev/null; then echo "identical"; else diff "$a" "$b" | head -60; fi ;;
*) sed -n '2,12p' "$0"; exit 2 ;;
esac
