#!/usr/bin/env bash
# Copy the game's non-binary files (data/, mods/, config.xml, version/branch files) from the user's install
# into the game directory the launcher runs in. Idempotent (rsync); binaries are copied separately.
# Usage: tools/setup_game.sh [source install dir]
#   source: $NOITA_SRC, default the Sikarugir Wine prefix's GOG install
#   destination: $NOITA_DIR, default build/game (gitignored: never commit game files)
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=${1:-${NOITA_SRC:-"$HOME/Applications/Noita Sikarugir.app/Contents/SharedSupport/prefix/drive_c/GOG Games/Noita"}}
DST=${NOITA_DIR:-build/game}
[ -f "$SRC/data/data.wak" ] || { echo "no Noita install at $SRC (data/data.wak missing)"; exit 1; }
mkdir -p "$DST"
case "$(cd "$DST" && pwd)/" in  # inside the repo, the destination must be gitignored (never commit game files)
    "$PWD"/*) git check-ignore -q "$DST" || { echo "refusing: $DST is inside the repo but not gitignored"; exit 1; } ;;
esac
rsync -a --delete "$SRC/data/" "$DST/data/"
rsync -a --delete "$SRC/mods/" "$DST/mods/"
for f in config.xml _branch.txt _version_hash.txt _release_notes.txt screenshot_paths.txt; do
    [ -f "$SRC/$f" ] && rsync -a "$SRC/$f" "$DST/$f"
done
echo "game data: $SRC -> $DST ($(du -sh "$DST" | cut -f1))"
