#!/bin/sh
# sly1-wine-smoke.sh BINDIR OUTDIR INSTALLATION [FRAMES] [KEYS]
#
# tools/sly1-smoke.sh for the Windows build under Proton (Linux host): BINDIR holds a MinGW cross build of
# Sly1-Release.exe linked with the smoke hooks (-DCANE_SLY1_SMOKE_HOOKS=ON) and llvm-mingw's libc++.dll, libunwind.dll
# and libwinpthread-1.dll beside it. The game runs through `proton run` in a private prefix (OUTDIR/../wine-prefix or
# SLY1_WINE_PREFIX), a private HOME and XDG profile, its window hidden on the X display SLY1_WINE_DISPLAY (required:
# Proton's Wine has only winex11; use a headless one, e.g. tools/sly1-headless-x.sh's gamescope), and its audio
# going to ALSA's null device only: winepulse is disabled, PulseAudio / PipeWire are unreachable (no PULSE_SERVER, a
# private XDG_RUNTIME_DIR) and the ALSA configuration has no device but the null sink, so the real XAudio2 (the
# bundled xaudio2_9redist.dll) mixes as on Windows and nothing reaches a speaker. The game's own capture of its final
# mix (the debug menu's test step mixtrace, SAFEHOUSE_DEBUGMENU) writes BINDIR/CameraDiagnostics/*.wav.
# INSTALLATION: an installed Sly 1 folder (extraction_manifest.json). The same KEYS / SAFEHOUSE_DEBUGMENU /
# The game's output (through cmd.exe's redirection: Proton's launcher hands the game no standard handles of its own)
# goes to OUTDIR/game.log, Wine's and Proton's to OUTDIR/wine.log. SLY1_SMOKE_DEBUG / SLY1_SMOKE_EVERY / SLY1_SMOKE_EXTRA / SLY1_SMOKE_ARGS / SLY1_SMOKE_CLOCK as tools/sly1-smoke.sh.
# PROTON: the proton script (default Steam's Proton 11.0).
# SLY1_WINE_TOOL="ARGS": instead of the game, the program's tool mode with these arguments (word-split; Windows paths,
# e.g. "--export-wav Z:/.../MUSID_59.mus Z:/.../out.wav --seconds 30"), INSTALLATION and FRAMES unused ("-"); exit 0 =
# the tool succeeded.
set -u
BIN=$(cd "$1" && pwd); OUT=$2; DATA=$3; FRAMES=${4:-4200}
TOOL=${SLY1_WINE_TOOL:-}
[ -n "$TOOL" ] || DATA=$(cd "$DATA" && pwd)
KEYS=${5:-257@1200-1206,257@1500-1506,32@1800-1806,257@2100-2106,32@2400-2406,32@2700-2706,32@3000-3006,32@3300-3306,32@3600-3606}
PROTON=${PROTON:-$HOME/.local/share/Steam/steamapps/common/Proton 11.0/proton}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
fail() { echo "FAIL: $*"; exit 1; }
[ -f "$BIN/Sly1-Release.exe" ] || fail "no $BIN/Sly1-Release.exe"
[ -n "$TOOL" ] || [ -f "$DATA/extraction_manifest.json" ] || fail "no Sly 1 installation at $DATA"
[ -f "$PROTON" ] || fail "no Proton at $PROTON"
W=${SLY1_WINE_PREFIX:-$(dirname "$OUT")/wine-prefix}
mkdir -p "$W/home/.config" "$W/home/.local/share" "$W/home/.cache" "$W/home/.local/state" "$W/rt" "$W/compat"
chmod 700 "$W/rt"
printf 'pcm.!default { type null }\npcm.null { type null }\n' > "$W/alsa.conf"
win() { printf 'Z:%s' "$1"; }
rm -rf "$OUT/user" "$OUT/shots"; mkdir -p "$OUT/user" "$OUT/shots"
[ "${SLY1_SMOKE_DEBUG:-0}" = 1 ] && printf '[Debug]\nDebugMode=1\n' > "$OUT/user/Debug Mode.ini"
# (the program's arguments)
if [ -n "$TOOL" ]; then set -- $TOOL; else set -- --data-dir "$(win "$DATA")" --user-dir "$(win "$OUT/user")" ${SLY1_SMOKE_ARGS:-}; fi
[ -n "${SLY1_WINE_DISPLAY:-}" ] || fail "set SLY1_WINE_DISPLAY to a headless X display (tools/sly1-headless-x.sh)"
(cd "$BIN" && env -u WAYLAND_DISPLAY -u PULSE_SERVER -u PULSE_CLIENTCONFIG -u PIPEWIRE_REMOTE \
    HOME="$W/home" XDG_CONFIG_HOME="$W/home/.config" XDG_DATA_HOME="$W/home/.local/share" \
    XDG_CACHE_HOME="$W/home/.cache" XDG_STATE_HOME="$W/home/.local/state" XDG_RUNTIME_DIR="$W/rt" \
    DISPLAY="$SLY1_WINE_DISPLAY" ALSA_CONFIG_PATH="$W/alsa.conf" WINEDLLOVERRIDES="winepulse.drv=d" \
    STEAM_COMPAT_DATA_PATH="$W/compat" STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam" \
    WINEDEBUG="${WINEDEBUG:--all}" SDL_AUDIODRIVER=dummy \
    SAFEHOUSE_SLY1_SMOKE="out=$(win "$OUT/shots");every=${SLY1_SMOKE_EVERY:-150};frames=$FRAMES;keys=$KEYS;clock=${SLY1_SMOKE_CLOCK:-swaps}${SLY1_SMOKE_EXTRA:+;$SLY1_SMOKE_EXTRA}" \
    timeout -s KILL 1200 nice -n 19 python3 "$PROTON" run cmd.exe /c Sly1-Release.exe "$@" \
    ">" "$(win "$OUT/game.log")" "2>&1" > "$OUT/wine.log" 2>&1)
E=$?
# (the game's _exit or the timeout leaves the prefix's wineserver and services behind: end them)
WINEPREFIX="$W/compat/pfx" "$(dirname "$PROTON")/files/bin/wineserver" -k 2>/dev/null
[ $E -eq 0 ] || fail "the game exited with $E (see $OUT/game.log)"
if [ -n "$TOOL" ]; then echo "PASS: $(tail -1 "$OUT/game.log")"; exit 0; fi
grep -q "^exit $FRAMES" "$OUT/shots/log.txt" || fail "no timed exit (see $OUT/shots/log.txt)"
echo "PASS: $FRAMES frames (installation $DATA; screenshots in $OUT/shots)"
