#!/usr/bin/env bash
# Copy the macOS FMOD Engine (the user's own download from fmod.com; never commit it) into build/fmod_api/:
# lib/libfmod.dylib, lib/libfmodstudio.dylib and inc/*.h. The launcher dlopens the dylibs at run time
# (runtime/fmod.c). Idempotent. Checks that the headers are FMOD 2.01.x (the game ships 2.01.05) and that
# both dylibs contain arm64.
# Usage: tools/setup_fmod.sh [installer dmg]
#   dmg: $FMOD_DMG, default ~/Downloads/fmodstudioapi20123mac-installer.dmg
#   destination: $FMOD_DIR, default build/fmod_api (gitignored; build/fmod is the fmod.dll module dir)
set -euo pipefail
cd "$(dirname "$0")/.."
DMG=${1:-${FMOD_DMG:-"$HOME/Downloads/fmodstudioapi20123mac-installer.dmg"}}
DST=${FMOD_DIR:-build/fmod_api}
[ -f "$DMG" ] || { echo "no FMOD installer at $DMG (download the FMOD Engine for Mac from fmod.com)"; exit 1; }
mkdir -p "$DST"
case "$(cd "$DST" && pwd)/" in  # inside the repo, the destination must be gitignored (never commit FMOD)
    "$PWD"/*) git check-ignore -q "$DST" || { echo "refusing: $DST is inside the repo but not gitignored"; exit 1; } ;;
esac
MNT=$(mktemp -d /tmp/fmod.XXXXXX)
trap 'hdiutil detach -quiet "$MNT" 2>/dev/null; rmdir "$MNT"' EXIT
hdiutil attach -readonly -nobrowse -noverify -quiet -mountpoint "$MNT" "$DMG"
API="$MNT/FMOD Programmers API/api"
[ -d "$API" ] || { echo "$DMG has no 'FMOD Programmers API/api'"; exit 1; }
mkdir -p "$DST/lib" "$DST/inc"
rsync -a "$API/core/lib/libfmod.dylib" "$API/studio/lib/libfmodstudio.dylib" "$DST/lib/"
rsync -a "$API/core/inc/"*.h "$API/studio/inc/"*.h "$DST/inc/"
version=$(sed -nE 's/^#define FMOD_VERSION +0x([0-9a-fA-F]+).*/\1/p' "$DST/inc/fmod_common.h")
case "$version" in
    000201??) ;;
    *) echo "FMOD_VERSION 0x$version: expected 2.01.x (0x000201xx)"; exit 1 ;;
esac
for lib in "$DST/lib/libfmod.dylib" "$DST/lib/libfmodstudio.dylib"; do
    lipo -archs "$lib" | tr ' ' '\n' | grep -qx arm64 || { echo "$lib has no arm64 slice"; exit 1; }
done
echo "FMOD 0x$version: $DMG -> $DST"
