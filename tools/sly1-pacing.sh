#!/bin/sh
# sly1-pacing.sh BINDIR OUTDIR (ISO | INSTALLATION) [RATES]
#
# Sly 1's frame-rate gate (Linux): the same smoke run (tools/sly1-smoke.sh) at each frame-rate cap, scheduled in game
# time (SLY1_SMOKE_CLOCK=game: the keys and the screenshots at the same moments of the game at every rate), VSync off.
# RATES: the caps (default "30 60 144 240 Uncapped"). Per rate OUTDIR/<rate>/ holds the run (shots/, game.log); the
# summary (OUTDIR/summary.txt) gives the frames per second reached, the game time per second of wall time (1.00: the
# game's speed is the real one) and the player's position at each screenshot, for the rates side by side.
# Exit 0: every run passed and its game time kept up with the wall clock (0.97 .. 1.03 while the game ran its frames).
set -u
BIN=$1; OUT=$2; SRC=$3; RATES=${4:-"30 60 144 240 Uncapped"}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
# (game milliseconds: logos and the title, a new game, its first level (Enter = Start); the intro's binocucom talk
#  skipped (Instant Skip Cutscenes on: a press of E = Circle skips the conversation), then W held from 60 s to 70 s: Sly runs, so the positions at 65, 70 and
#  75 s compare the physics at each cap. Space is Cross in the default layout since the input step.)
GAME_MS=${SLY1_PACING_MS:-75000}
KEYS=${SLY1_PACING_KEYS:-257@20000-20150,257@25000-25150$(for t in 30000 32000 34000 36000 38000 40000 42000 44000 46000 48000 50000 52000 54000; do printf ',69@%d-%d' $t $((t + 150)); done),87@60000-70000}
# (one installation for all the runs: the first run installs from an ISO)
DATA=$SRC
status=0
for rate in $RATES; do
    dir=$OUT/$rate
    mkdir -p "$dir"
    printf '[Display]\nFrame Rate Limit = %s\nVSync = Off\n[Controls]\nInstant Skip Cutscenes = On\n' "$rate" > "$dir/Settings.ini"
    SLY1_SMOKE_CLOCK=game SLY1_SMOKE_EVERY=${SLY1_PACING_EVERY:-5000} SLY1_SMOKE_SETTINGS=$dir/Settings.ini \
        "$HERE/sly1-smoke.sh" "$BIN" "$dir" "$DATA" "$GAME_MS" "$KEYS" > "$dir/smoke.txt" 2>&1 || { echo "FAIL: $rate (see $dir/smoke.txt)"; status=1; continue; }
    if [ -f "$DATA" ]; then
        DATA=$(find "$dir/profile/data" -name extraction_manifest.json -path '*sly1*' | head -1 | xargs -r dirname)
    fi
done
{
    for rate in $RATES; do
        log=$OUT/$rate/shots/log.txt
        [ -f "$log" ] || continue
        # (the medians of the one-second stats after the first 10 s: a second with a level load in it, when the game
        #  clock stands still, is not the cap's)
        awk -v rate="$rate" '
            /^stat/ { for (i = 2; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
                      n++; if (n > 10) { c++; f[c] = v["fps"] + 0; r[c] = v["rate"] + 0 } }
            END { if (!c) { printf "%-9s no stats\n", rate; exit }
                  asort(f); asort(r); m = int((c + 1) / 2)
                  printf "%-9s fps %7.1f  game time per second %.3f  (%d s measured)\n", rate, f[m], r[m], c }' "$log"
    done
    echo
    echo "the player at each screenshot (game time: position)"
    for rate in $RATES; do
        log=$OUT/$rate/shots/log.txt
        [ -f "$log" ] || continue
        printf '%s\n' "$rate"
        awk '/^shot/ { g = ""; p = "-"; for (i = 2; i <= NF; i++) { if ($i ~ /^game=/) g = substr($i, 6); if ($i ~ /^player=/) p = substr($i, 8) }
                       printf "  %8s  %s\n", g, p }' "$log"
    done
} > "$OUT/summary.txt"
cat "$OUT/summary.txt"
awk '/game time per second/ { if ($8 < 0.97 || $8 > 1.03) bad = 1 } END { exit bad }' "$OUT/summary.txt" || { echo "FAIL: the game time drifted from the wall clock"; status=1; }
[ $status -eq 0 ] && echo "PASS: $RATES"
exit $status
