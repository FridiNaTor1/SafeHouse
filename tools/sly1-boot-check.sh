#!/bin/sh
# sly1-boot-check.sh BINDIR OUTDIR [ISO | INSTALLATION]
#
# Sly 1's boot regression check (2026-11-05, the black screen after the first movie):
#  1. with an ISO: a fresh install through the launcher (BINDIR/safehouse --install, then --verify sly1) into a
#     private XDG profile under OUTDIR/profile; with an installation folder: that folder as it is (read only);
#  2. boot with no keys pressed (the tester's case): the Sucker Punch movie, then Paris' opening scene and the title;
#  3. boot again and press Cross through the opening's binocucom dialogue (the first level's gameplay).
# Each run must end in Paris with a picture in the WINDOW (--smoke-require-picture compares the composited back
# buffer with the scene), and the opening scene must have been drawn (SLY1_SHOTEVERY frames, uis 3 = playing).
# Needs a display (GLFW window; X11 or Wayland), audio muted (SDL dummy driver). Private dirs only: OUTDIR/profile,
# OUTDIR/user-*. Exit 0 = PASS.
set -u
BIN=$(cd "$1" && pwd); OUT=$2; SRC=${3:-}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
export SDL_AUDIODRIVER=dummy
fail() { echo "FAIL: $*"; exit 1; }

if [ -z "$SRC" ]; then fail "give an ISO or an installation folder"; fi
if [ -f "$SRC" ]; then
    rm -rf "$OUT/profile"; mkdir -p "$OUT/profile"
    # the launcher's own install, in a private profile (never ~/.config/SafeHouse)
    XDG_CONFIG_HOME=$OUT/profile/config XDG_DATA_HOME=$OUT/profile/data XDG_STATE_HOME=$OUT/profile/state \
        "$BIN/safehouse" --install "$SRC" > "$OUT/install.log" 2>&1 || fail "install (see $OUT/install.log)"
    XDG_CONFIG_HOME=$OUT/profile/config XDG_DATA_HOME=$OUT/profile/data XDG_STATE_HOME=$OUT/profile/state \
        "$BIN/safehouse" --verify sly1 >> "$OUT/install.log" 2>&1 || fail "verify (see $OUT/install.log)"
    DATA=$OUT/profile/data/SafeHouse/installations/sly1
else
    DATA=$(cd "$SRC" && pwd)
fi
[ -f "$DATA/extraction_manifest.json" ] || fail "no Sly 1 installation at $DATA"

# run TAG SECONDS [ARGS...]
run() {
    T=$1; S=$2; shift 2
    rm -rf "$OUT/user-$T" "$OUT/shots-$T"; mkdir -p "$OUT/user-$T" "$OUT/shots-$T"
    (cd "$BIN" && SLY1_SHOTEVERY="$OUT/shots-$T:300" timeout -s KILL $((S + 90)) ./Sly1-Release --data-dir "$DATA" \
        --user-dir "$OUT/user-$T" --smoke-test-seconds "$S" --smoke-require-picture "$@" > "$OUT/$T.log" 2>&1)
    E=$?
    grep -E "^(SMOKE|PRESENT|SafeHouse):" "$OUT/$T.log" | sed "s/^/  $T: /"
    [ $E -eq 0 ] || fail "$T: exit $E (see $OUT/$T.log)"
    grep -q "^SMOKE: .*world=Paris" "$OUT/$T.log" || fail "$T: did not end in Paris"
    # the opening scene: Paris drawn while playing (uis 3), after the movie
    grep -q "^SHOT: .*world=Paris.* uis=3 " "$OUT/$T.log" || fail "$T: Paris' opening scene never ran"
}

run idle 60                                           # no keys: movie -> Paris opening -> title
run play 75 --smoke-press 6@1700,6@1800,6@1900,6@2000,6@2100,6@2200   # Cross through the binocucom lines
echo "PASS (installation $DATA; frames in $OUT/shots-*)"
