#!/bin/sh
# sly1-smoke.sh BINDIR OUTDIR (ISO | INSTALLATION) [FRAMES] [KEYS]
#
# Sly 1's smoke run for the game from the Sly1/ submodule (Linux): through the launcher's own install when given an ISO
# (BINDIR/safehouse --install, --verify sly1, in a private HOME and XDG profile under OUTDIR/profile), then the game in
# a hidden window with scripted keys and a screenshot every 150 frames (tools/sly1-smoke-hooks.cpp,
# SAFEHOUSE_SLY1_SMOKE), muted (SDL dummy audio driver), its user folder OUTDIR/user. Needs a display (X11/Wayland),
# never shows a window. The default keys press Escape (Start: the original's skip of the opening scene;
# Enter is the cutscene skip's key there), Enter (Start) and Space (Cross) now and then: past the logos and the title,
# into a new game and its first level. Exit 0 = the game ran FRAMES frames; check OUTDIR/shots and OUTDIR/game.log.
# SLY1_SMOKE_EXTRA: more of tools/sly1-smoke-hooks.cpp's items (mouse, move, cursor, wheel, pad, padbtn, padaxis, shots,
# size), e.g. "pad=ps5;padbtn=0@1800-1806"; the game's own test switches (SLY1_PCOPT, SLY1_REMAPPAGE, ...) pass through.
# SLY1_SMOKE_ARGS: more arguments for the game (word-split), e.g. "--texture-pack DIR" for the PS3 HD textures.
# SLY1_SMOKE_CLOCK=game: FRAMES, the keys and the screenshots (SLY1_SMOKE_EVERY, default 150) in milliseconds of game
# time (tools/sly1-smoke-hooks.cpp clock=game). SLY1_SMOKE_SETTINGS=FILE: the run's Settings.ini (else the defaults).
# SLY1_RENDERER=vulkan / opengl picks the renderer (Auto otherwise), SLY1_SMOKE_SIZE=WxH the window's size (3840x1080:
# 32:9), SLY1_SMOKE_DEBUG=1 the game's debug mode (F3, key 292, opens its developer window; F10, key 299, the debug menu).
# SLY1_SMOKE_AUDIO=FILE: the game's mixed output written to FILE (SDL's disk audio driver: raw samples in the device's
# format, see the game.log's audio line) instead of the dummy driver; still nothing reaches a speaker.
set -u
BIN=$(cd "$1" && pwd); OUT=$2; SRC=$3; FRAMES=${4:-4200}
KEYS=${5:-256@1200-1206,257@1500-1506,32@1800-1806,257@2100-2106,32@2400-2406,32@2700-2706,32@3000-3006,32@3300-3306,32@3600-3606}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
fail() { echo "FAIL: $*"; exit 1; }
P=$OUT/profile
mkdir -p "$P/home" "$P/config" "$P/data" "$P/cache" "$P/state" "$P/runtime"; chmod 700 "$P/runtime"
private() {
    HOME=$P/home XDG_CONFIG_HOME=$P/config XDG_DATA_HOME=$P/data XDG_CACHE_HOME=$P/cache XDG_STATE_HOME=$P/state \
    XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-$P/runtime} SDL_AUDIODRIVER=dummy "$@"
}
AUDIO=
[ -n "${SLY1_SMOKE_AUDIO:-}" ] && AUDIO="SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE=$SLY1_SMOKE_AUDIO"

if [ -f "$SRC" ]; then
    private "$BIN/safehouse" --install "$SRC" > "$OUT/install.log" 2>&1 || fail "install (see $OUT/install.log)"
    private "$BIN/safehouse" --verify sly1 >> "$OUT/install.log" 2>&1 || fail "verify (see $OUT/install.log)"
    DATA=$(find "$P/data" -name extraction_manifest.json -path '*sly1*' | head -1 | xargs -r dirname)
else
    DATA=$(cd "$SRC" && pwd)
fi
[ -n "$DATA" ] && [ -f "$DATA/extraction_manifest.json" ] || fail "no Sly 1 installation (see $OUT/install.log)"
rm -rf "$OUT/user" "$OUT/shots"; mkdir -p "$OUT/user" "$OUT/shots"
# SLY1_SMOKE_USER_SEED: a folder copied into the fresh user folder first (e.g. a content_settings.json: the language)
[ -n "${SLY1_SMOKE_USER_SEED:-}" ] && cp -r "$SLY1_SMOKE_USER_SEED"/. "$OUT/user/"
[ -n "${SLY1_SMOKE_SETTINGS:-}" ] && cp "$SLY1_SMOKE_SETTINGS" "$OUT/user/Settings.ini"
[ "${SLY1_SMOKE_DEBUG:-0}" = 1 ] && printf '[Debug]\nDebugMode=1\n' > "$OUT/user/Debug Mode.ini"
(cd "$BIN" && private env $AUDIO SAFEHOUSE_SLY1_SMOKE="out=$OUT/shots;every=${SLY1_SMOKE_EVERY:-150};frames=$FRAMES;keys=$KEYS;clock=${SLY1_SMOKE_CLOCK:-swaps}${SLY1_SMOKE_SIZE:+;size=$SLY1_SMOKE_SIZE}${SLY1_SMOKE_EXTRA:+;$SLY1_SMOKE_EXTRA}" \
    timeout -s KILL 900 ./Sly1-Release --data-dir "$DATA" --user-dir "$OUT/user" ${SLY1_SMOKE_ARGS:-} > "$OUT/game.log" 2>&1)
E=$?
[ $E -eq 0 ] || fail "the game exited with $E (see $OUT/game.log)"
grep -q "^exit $FRAMES" "$OUT/shots/log.txt" || fail "no timed exit (see $OUT/shots/log.txt)"
echo "PASS: $FRAMES frames (installation $DATA; screenshots in $OUT/shots)"
