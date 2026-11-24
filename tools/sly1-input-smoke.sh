#!/bin/sh
# sly1-input-smoke.sh BINDIR OUTDIR INSTALLATION [CASES]
#
# Sly 1's input runs (Linux), each a tools/sly1-smoke.sh run on an installed game (tools/sly1-smoke.sh ISO first):
#   keyboard   the default keyboard layout into the first level (Enter, Space), then W held (walk), Space (jump), the
#              arrow keys (camera); the pause menu (Escape) and the Controls page (screenshots)
#   mouse      the mouse camera: captured while playing (the hooks' log "cursor N captured"), moved; the pause menu by
#              mouse (hover + left click on a row), the right button back
#   pad-FAMILY a virtual controller of each family (xbox ps3 ps4 ps5 switch deck steamcontroller) into the first
#              level and the Controls page: the icons of that family (the test strip SLY1_PROMPTSTRIP and the page)
#   icons-N    the keyboard run with the Button Icons option N (SLY1_PCOPT promptstyle=N; alwaysps for the original
#              PS2 icons): the strip in each icon set
#   remap      the Controls page by keyboard: Space rebinds Jump to J, then J jumps (SLY1_REMAPLOG: the binding line)
# Screenshots land in OUTDIR/CASE/shots; a case fails when its run does not reach its last frame or a check fails.
set -u
BIN=$1; OUT=$2; INST=$3
CASES=${4:-"keyboard mouse remap pad-xbox pad-ps3 pad-ps4 pad-ps5 pad-switch pad-deck pad-steamcontroller icons-0 icons-1 icons-2 icons-3 icons-4 icons-5 icons-6 icons-7 icons-ps2"}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
fails=0
# Into the first level: the title (Enter twice: the first press wakes the title menu), then the intro's binocucom
# talk skipped line by line (R1: Left Shift); Sly is playable from about swap 4500.
INTO="257@1500-1504,257@1800-1804"
PADINTO="7@1500-1504,7@1800-1804"
f=2000
while [ $f -lt 4440 ]; do INTO="$INTO,340@$f-$((f + 3))"; PADINTO="$PADINTO,5@$f-$((f + 3))"; f=$((f + 40)); done
run() {   # run CASE FRAMES KEYS [ENV...]
    c=$1; frames=$2; keys=$3; shift 3
    if env "$@" "$HERE/sly1-smoke.sh" "$BIN" "$OUT/$c" "$INST" "$frames" "$keys" > "$OUT/$c.log" 2>&1; then
        echo "PASS: $c"
    else
        echo "FAIL: $c (see $OUT/$c.log)"; fails=$((fails + 1))
    fi
}
check() {  # check CASE DESCRIPTION COMMAND...
    c=$1; d=$2; shift 2
    if "$@"; then echo "  ok: $c: $d"; else echo "  FAIL: $c: $d"; fails=$((fails + 1)); fi
}
for c in $CASES; do
    case $c in
    keyboard)
        # in the level: W held 4700-5000, Space at 4800, the camera keys 5100-5200, Escape (pause) 5400: the Controls
        # page (the test switch opens it with the pause menu) and its screenshots
        run "$c" 5700 "$INTO,87@4700-5000,32@4800-4806,263@5100-5200,256@5400-5406" \
            SLY1_SMOKE_EVERY=0 SLY1_SMOKE_EXTRA="shots=4690,4810,5010,5210,5500,5650" SLY1_REMAPPAGE=1
        ;;
    mouse)
        # the camera: moved right 4700-4900 (captured while playing); Escape 5100: the pause menu (the cursor free);
        # the cursor on its second row (Options...) and a left click (the options menu), the right button (back)
        run "$c" 5900 "$INTO,256@5100-5106" SLY1_SMOKE_EVERY=0 \
            SLY1_SMOKE_EXTRA="move=12:0@4700-4900;cursor=640:358@5200-5900;mouse=0@5400-5404,1@5600-5604;shots=4690,4910,5300,5500,5700"
        [ -f "$OUT/$c/shots/log.txt" ] && check "$c" "cursor captured while playing" grep -q "captured" "$OUT/$c/shots/log.txt"
        [ -f "$OUT/$c/shots/log.txt" ] && check "$c" "cursor freed in the pause menu" grep -q "free" "$OUT/$c/shots/log.txt"
        ;;
    remap)
        # Escape at 4200 opens the pause menu and the Controls page (SLY1_REMAPPAGE); Down x12 to Jump (Cross: past the
        # six options, the heading, Move x4, Attack, Action), Enter
        # starts the capture, J (74) binds it; Escape closes the page and the menu; J then jumps.
        k="$INTO,256@4700-4706"
        f=4900; i=0
        while [ $i -lt 12 ]; do k="$k,264@$f-$((f + 2))"; f=$((f + 20)); i=$((i + 1)); done
        k="$k,257@$((f + 20))-$((f + 24)),74@$((f + 80))-$((f + 84)),256@$((f + 160))-$((f + 164)),256@$((f + 240))-$((f + 244)),74@$((f + 400))-$((f + 406))"
        run "$c" $((f + 700)) "$k" SLY1_SMOKE_EVERY=0 SLY1_REMAPPAGE=1 SLY1_REMAPLOG=1 \
            SLY1_SMOKE_EXTRA="shots=$((f + 10)),$((f + 120)),$((f + 300)),$((f + 420))"
        check "$c" "the capture bound a key" grep -q "remap: row .* key <- 74" "$OUT/$c/game.log"
        ;;
    pad-*)
        fam=${c#pad-}
        # the strip over everything; Start (7) at 4200 opens the pause menu and the Controls page
        run "$c" 5100 "0@0-0" SLY1_SMOKE_EVERY=0 SLY1_PROMPTSTRIP=1 SLY1_REMAPPAGE=1 \
            SLY1_SMOKE_EXTRA="pad=$fam;padbtn=$PADINTO,7@4700-4706;shots=1400,4600,5000"
        ;;
    icons-*)
        n=${c#icons-}
        if [ "$n" = ps2 ]; then opt="alwaysps=1"; else opt="promptstyle=$n"; fi
        run "$c" 5000 "$INTO,256@4700-4706" SLY1_SMOKE_EVERY=0 SLY1_PROMPTSTRIP=1 SLY1_REMAPPAGE=1 SLY1_PCOPT="$opt" \
            SLY1_SMOKE_EXTRA="shots=1600,4600,4900"
        ;;
    esac
done
[ $fails -eq 0 ] && echo "PASS: all input cases" || echo "FAIL: $fails"
exit $fails
