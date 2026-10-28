#!/usr/bin/env bash
# Build Noita.app from the user's own Noita install. The recompiled code is derived from the game, so it is
# never shipped: this runs the discover/lift/build pipeline locally against the install's noita.exe and
# msvcp120.dll, then wraps build/mina4mac in an app bundle that runs the game from that install (it copies
# no game files; binaries and data/ are read in place, and logger.txt is written there, as on Windows).
# Saves live under ~/Library/Application Support/mina4mac/. The launcher log goes to ~/Library/Logs/mina4mac.log
# (the previous launch's in mina4mac.log.1).
# Usage: tools/package_app.sh [--no-build] [--fps] [--telemetry] [install dir]
#   install: $NOITA_SRC, default the Sikarugir Wine prefix's GOG install (as tools/setup_game.sh)
#   $MINA4MAC_BUILD_ARGS: extra tools/build_all.py arguments, e.g. "--sync check"
#   output: $MINA4MAC_APP, default build/Noita.app (gitignored; copy it anywhere, e.g. ~/Applications)
#   --no-build: reuse the existing build/mina4mac (it must have been built from the same exe)
#   --fps: the app shows fps and the worst frame time in the window title (MINA4MAC_FPS=1; not in fullscreen)
#   --telemetry: --fps, plus the playtest mod tools/telemetry, installed into the install's mods/ and enabled in
#     the app's save, so each [fps] line in the log also names the frame, biome, position and nearby entities
#     (the game drops mods it can't find from the save's list, so run this again after benchmark runs in build/game)
# Bundles Homebrew's SDL2 (sdl2-compat, plus the SDL3 it loads) and, if tools/setup_fmod.sh ran, the FMOD dylibs (else audio is the silent stub).
# For the Steam build, also libsteam_api.dylib if tools/setup_steam.sh ran (else the game runs offline).
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=1 FPS= TELEMETRY=0
while [ $# -gt 0 ]; do
    case "$1" in
    --no-build) BUILD=0 ;;
    --fps) FPS='export MINA4MAC_FPS="${MINA4MAC_FPS:-1}"' ;;
    --telemetry) FPS='export MINA4MAC_FPS="${MINA4MAC_FPS:-1}"' TELEMETRY=1 ;;
    *) break ;;
    esac
    shift
done
SRC=${1:-${NOITA_SRC:-"$HOME/Applications/Noita Sikarugir.app/Contents/SharedSupport/prefix/drive_c/GOG Games/Noita"}}
APP=${MINA4MAC_APP:-build/Noita.app}

# The runtime is written against the Jan 25 2025 build of the game, from GOG or Steam (tools/pe.py BUILDS):
# check the binaries it recompiles.
sha() { [ -f "$SRC/$1" ] || { echo "no Noita install at $SRC ($1 missing)"; exit 1; }; shasum -a 256 "$SRC/$1" | cut -d' ' -f1; }
EXE_SHA=$(sha noita.exe)
case $EXE_SHA in
f0f5ffd1c64558663ac7cffaf689f4899732a56494c3530b806bf3357d094363) GAME_BUILD=GOG ;;
808d2a0ab51ea0b46e9ad2aeb3327a4b0ce3feae04f32ba26326bf585b5779bd) GAME_BUILD=Steam ;;
*) echo "$SRC/noita.exe is not a supported build (Noita, Build Jan 25 2025, GOG or Steam)"; exit 1 ;;
esac
[ "$(sha msvcp120.dll)" = 87c42ca155473e4e71857d03497c8cbc28fa8ff7f2c8d72e8a1f39b71078f608 ] ||
    { echo "$SRC/msvcp120.dll is not the supported build"; exit 1; }
[ -f "$SRC/data/data.wak" ] || { echo "no Noita install at $SRC (data/data.wak missing)"; exit 1; }
SRC=$(cd "$SRC" && pwd)
made_here() {  # made by this script (NoitamacGameDir: before the project was renamed)
    local k; for k in Mina4macGameDir NoitamacGameDir; do
        /usr/libexec/PlistBuddy -c "Print :$k" "$APP/Contents/Info.plist" >/dev/null 2>&1 && return 0
    done; return 1
}
if [ -e "$APP" ] && ! made_here; then
    echo "refusing to overwrite $APP: it wasn't made by this script"; exit 1
fi

if [ $BUILD = 1 ]; then
    export NOITA_DIR="$SRC"  # tools/pe.py reads the modules from here
    # Cached analysis (discover.pkl, regsum.pkl, ...) is for one noita.exe: drop it when the exe changes.
    [ "$(cat build/noita/exe.sha256 2>/dev/null)" = "$EXE_SHA" ] || rm -rf build/noita
    mkdir -p build/noita && echo "$EXE_SHA" >build/noita/exe.sha256
    for m in noita msvcp120; do
        [ -f "build/$m/discover.pkl" ] || uv run tools/discover.py "$m"
    done
    uv run tools/build_all.py ${MINA4MAC_BUILD_ARGS:-}
fi
[ -x build/mina4mac ] || { echo "build/mina4mac is missing (run without --no-build)"; exit 1; }

TMP="$APP.tmp"
rm -rf "$TMP"
mkdir -p "$TMP/Contents/MacOS" "$TMP/Contents/Frameworks" "$TMP/Contents/Resources"
C="$TMP/Contents"
cp build/mina4mac "$C/MacOS/mina4mac"
sdl=$(otool -L build/mina4mac | awk '/libSDL2/ {print $1}')
cp -L "$sdl" "$C/Frameworks/"
chmod u+w "$C/Frameworks/$(basename "$sdl")"
install_name_tool -id "@executable_path/../Frameworks/$(basename "$sdl")" "$C/Frameworks/$(basename "$sdl")"
install_name_tool -change "$sdl" "@executable_path/../Frameworks/$(basename "$sdl")" "$C/MacOS/mina4mac"
# Homebrew's sdl2 is sdl2-compat, which dlopens SDL3 at runtime (not an otool dependency); among other places it
# looks for @loader_path/libSDL3.dylib, so that is where SDL3 goes.
if strings "$sdl" | grep -q '@loader_path/libSDL3.dylib'; then
    sdl3=$(dirname "$sdl")/libSDL3.dylib
    [ -f "$sdl3" ] || sdl3="$(brew --prefix sdl3)/lib/libSDL3.dylib"
    [ -f "$sdl3" ] || { echo "$sdl is sdl2-compat, but there is no SDL3 to bundle (brew install sdl3)"; exit 1; }
    cp -L "$sdl3" "$C/Frameworks/libSDL3.dylib"
    chmod u+w "$C/Frameworks/libSDL3.dylib"
    install_name_tool -id "@loader_path/libSDL3.dylib" "$C/Frameworks/libSDL3.dylib"
fi
fmod=no
if [ -f build/fmod_api/lib/libfmodstudio.dylib ]; then
    cp build/fmod_api/lib/libfmod.dylib build/fmod_api/lib/libfmodstudio.dylib "$C/Frameworks/"
    fmod=yes
fi
steam=no
if [ "$GAME_BUILD" = Steam ] && [ -f build/steam_api/libsteam_api.dylib ]; then
    cp build/steam_api/libsteam_api.dylib "$C/Frameworks/"
    steam=yes
fi
icon=$(ls "$SRC"/goggame-*.ico 2>/dev/null | head -1 || true)
[ -n "$icon" ] && sips -s format icns "$icon" --out "$C/Resources/Noita.icns" >/dev/null

# The bundle's executable: point the launcher at the install, keep the display awake (with it asleep,
# SDL_GL_SwapWindow blocks on vsync) and log to ~/Library/Logs. NOITA_DIR/MINA4MAC_* from the environment win.
q=$(printf %q "$SRC")
cat >"$C/MacOS/Noita" <<EOF
#!/bin/bash
# Generated by mina4mac's tools/package_app.sh.
C="\$(cd "\$(dirname "\$0")/.." && pwd)"
GAME=$q  # the install this app was built from
export NOITA_DIR="\${NOITA_DIR:-\$GAME}"
if [ ! -f "\$NOITA_DIR/noita.exe" ]; then
    /usr/bin/osascript -e "display alert \"Noita install not found\" message \"No noita.exe in \$NOITA_DIR. Rebuild the app with tools/package_app.sh.\"" >/dev/null
    exit 1
fi
[ -f "\$C/Frameworks/libfmodstudio.dylib" ] && export MINA4MAC_FMOD="\${MINA4MAC_FMOD:-\$C/Frameworks}"
[ -f "\$C/Frameworks/libsteam_api.dylib" ] && export MINA4MAC_STEAM="\${MINA4MAC_STEAM:-\$C/Frameworks}"
$FPS
LOG="\$HOME/Library/Logs/mina4mac.log"
mkdir -p "\$HOME/Library/Logs"
[ -f "\$LOG" ] && mv -f "\$LOG" "\$LOG.1"  # keep the previous launch's log (e.g. a crash) for one more launch
exec /usr/bin/caffeinate -d -u "\$C/MacOS/mina4mac" "\$@" >"\$LOG" 2>&1
EOF
chmod +x "$C/MacOS/Noita"

cat >"$C/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>Noita</string>
    <key>CFBundleIdentifier</key><string>local.mina4mac.Noita</string>
    <key>CFBundleName</key><string>Noita</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleIconFile</key><string>Noita</string>
    <key>CFBundleShortVersionString</key><string>$(git rev-parse --short HEAD)</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>Mina4macGameDir</key><string>$(sed 's/&/\&amp;/g; s/</\&lt;/g' <<<"$SRC")</string>
</dict>
</plist>
EOF

# install_name_tool invalidated the linker's ad-hoc signatures; Apple silicon won't run unsigned code.
codesign -s - -f "$C/Frameworks/"*.dylib "$C/MacOS/mina4mac"
codesign -s - -f "$TMP"
rm -rf "$APP"
mv "$TMP" "$APP"
echo "$APP: game $SRC ($GAME_BUILD), FMOD $fmod, Steam $steam ($(du -sh "$APP" | cut -f1))"

# The app's save (AppData under ~/Library/Application Support/mina4mac, as runtime/shell32.c maps it).
SAVE="$HOME/Library/Application Support/mina4mac/AppData/LocalLow/Nolla_Games_Noita/save00/mod_config.xml"
if [ $TELEMETRY = 1 ]; then
    rsync -a --delete tools/telemetry/ "$SRC/mods/mina4mac_telemetry/"
    if [ ! -f "$SAVE" ]; then
        echo "telemetry: installed, but there is no $SAVE yet: start the app once, quit, and run this again"
    elif grep -q 'name="mina4mac_telemetry"' "$SAVE"; then
        sed -i '' -E 's/<Mod enabled="[01]" name="mina4mac_telemetry"/<Mod enabled="1" name="mina4mac_telemetry"/' "$SAVE"
    else
        sed -i '' 's#</Mods>#  <Mod enabled="1" name="mina4mac_telemetry" settings_fold_open="0" workshop_item_id="0" >\
  </Mod>\
</Mods>#' "$SAVE"
    fi
    echo "telemetry: mods/mina4mac_telemetry in $SRC, enabled in the app's save"
fi
# The benchmark mods pin the seed (perfbench also floods the world): tools/perfbench.sh enables its mod only
# while it runs, but a killed run can leave it on; seedprint stays on until tools/determinism.sh remove.
for m in perfbench seedprint; do
    [ -d "$SRC/mods/$m" ] && grep -q "<Mod enabled=\"1\" name=\"$m\"" "$SAVE" 2>/dev/null &&
        echo "warning: the $m mod is installed in $SRC and enabled in the app's save (disable it in the mod menu or with its tool's remove)"
done
true
