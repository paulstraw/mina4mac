#!/usr/bin/env bash
# Copy the macOS libsteam_api.dylib (from the user's own Steamworks SDK download, partner.steamgames.com; never
# commit it) to build/steam_api/, where runtime/steam.c loads it from and tools/package_app.sh bundles it from
# (Steam build only). It must include arm64: SDK 1.52 or later (1.57 is tested; 1.58 changed SteamAPI_Init).
# Usage: tools/setup_steam.sh <sdk>/redistributable_bin/osx/libsteam_api.dylib
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=${1:?usage: tools/setup_steam.sh <path to libsteam_api.dylib>}
[ -f "$SRC" ] || { echo "no file at $SRC"; exit 1; }
lipo -archs "$SRC" | grep -qw arm64 ||
    { echo "$SRC has no arm64 code ($(lipo -archs "$SRC")): use the dylib from Steamworks SDK 1.52 or later (1.57 is tested)"; exit 1; }
syms=$(nm -gU -arch arm64 "$SRC")  # not piped into grep -q: with pipefail, nm's SIGPIPE would fail the check
for sym in SteamAPI_Init SteamInternal_FindOrCreateUserInterface SteamAPI_ManualDispatch_GetNextCallback; do
    grep -q " _$sym\$" <<<"$syms" ||
        { echo "$SRC has no $sym (SDK 1.58 and later replaced SteamAPI_Init: use 1.57)"; exit 1; }
done
mkdir -p build/steam_api
cp "$SRC" build/steam_api/libsteam_api.dylib
echo "libsteam_api ($(lipo -archs "$SRC")): $SRC -> build/steam_api"
