#!/bin/sh
# sly1-ready.sh BINDIR OUTDIR [STEPS]
#
# Sly 1's "ready" regression (notes/SLY1_READY.md, Linux desktop): every smoke tool at several points of the game, in
# one run, with a summary. Each step is a tools/sly1-smoke.sh run (hidden window, muted, private HOME / XDG profile and
# user folder under OUTDIR, scripted keys, screenshots) or one of the other Sly 1 gates:
#   regions   every region's disc installed through the launcher (safehouse --install, --verify sly1) and played into
#             Paris: USA, Europe, Japan, Korea (the discs below; a missing one is SKIP)
#   render    OpenGL and Vulkan, each at 16:9 (1280x720) and 32:9 (3840x1080), into Paris' first binocucom talk
#   places    the debug menu's warp (SAFEHOUSE_DEBUGMENU warp=LEVEL) into each place below, Debug mode on; the log's
#             scene (world, the player's vehicle / minigame) must be the place's and the picture not black
#   pacing    tools/sly1-pacing.sh: every frame-rate cap (30 60 90 120 144 240 Uncapped), the game's speed the same
#   ps3hd     the PS3 HD texture pack built from The Sly Collection (--ps3-hd-textures) and played with --texture-pack
#   input     tools/sly1-input-smoke.sh: keyboard, mouse, the Controls page's remap, a pad of each family, every icon set
#   tools     the music tools (--make-mus, --export-wav), --dump-text and --dump-textures DIR LEVEL
#   minigames the rhythm game (Mz. Ruby's fight: A Deadly Dance's checkpoint; its notes played with W, A, D and Space,
#             every one hit) and a vault (Duel by the Dragon: its clue
#             bottles given, the code 2-3-1 entered on the pad's d-pad; ProjectCane #45)
#   bugs      the regressions of the bug round (notes/SLY1_BUGS_2026-11-24.md): ProjectCane #57 (a charm swirling while
#             a door fires), #11 (the sprint's finish), #48 (no OpenGL error after cane swings, OpenGL and Vulkan),
#             #50 (the music's tempo with Fast), #51 (the Hideout's music while paused), #74 (the roosters meet and
#             explode at 60 and 240 FPS), #49 (High Class Heist's water section not loud), a level that fails to load
#   windows   (not in the default STEPS) the Windows build under Proton (SLY1_READY_WINBIN: a MinGW cross build with
#             -DCANE_SLY1_SMOKE_HOOKS=ON and llvm-mingw's DLLs beside it; tools/sly1-wine-smoke.sh on a headless X display
#             from tools/sly1-headless-x.sh, audio to ALSA's null device): ProjectCane #49 (High Class Heist's water
#             section, a teleport tour past the alarms, the real XAudio2's final mix captured: no Studio C reverb howl)
#             and --export-wav (a valid 48 kHz stereo WAV of a game track, not silent)
# STEPS: a subset, e.g. "regions places" (default: all, in that order). The regions step installs the USA disc first;
# the later steps play that installation (or SLY1_READY_INSTALL=<an installation folder>: no USA install).
# Discs (override with the variables): SLY1_ISO_USA, SLY1_ISO_EUROPE, SLY1_ISO_JAPAN, SLY1_ISO_KOREA, and
# SLY1_ISO_COLLECTION (The Sly Collection, PS3, for the HD textures).
# Output: OUTDIR/<step>/<case>/ (each run's shots/, game.log), OUTDIR/summary.txt (PASS / FAIL / SKIP per case).
# Exit 0: no case failed.
set -u
BIN=$(cd "$1" && pwd); OUT=$2; STEPS=${3:-"regions render places pacing ps3hd input tools minigames bugs"}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
# (a machine's own disc paths: SLY1_READY_ENV, or sly1-ready.env in the folder beside this checkout's parent, sourced
# when it exists; e.g. SLY1_ISO_USA="/path/to/Sly Cooper (USA).iso")
for env in "${SLY1_READY_ENV:-}" "$HERE/../../leadtools/sly1-ready.env"; do
    [ -n "$env" ] && [ -f "$env" ] && . "$env" && break
done
ISO_USA=${SLY1_ISO_USA:-/none}
ISO_EUROPE=${SLY1_ISO_EUROPE:-/none}
ISO_JAPAN=${SLY1_ISO_JAPAN:-/none}
ISO_KOREA=${SLY1_ISO_KOREA:-/none}
ISO_COLLECTION=${SLY1_ISO_COLLECTION:-/none}
SUMMARY=$OUT/summary.txt
: > "$SUMMARY"
fails=0
note() { echo "$*" | tee -a "$SUMMARY"; }
pass() { note "PASS  $1  $2"; }
fail() { note "FAIL  $1  $2"; fails=$((fails + 1)); }
skip() { note "SKIP  $1  $2"; }
installation() { find "$1/profile/data" -name extraction_manifest.json -path '*sly1*' 2>/dev/null | head -1 | xargs -r dirname; }
# the last screenshot's log line of a smoke run
lastshot() { grep '^shot ' "$1/shots/log.txt" 2>/dev/null | tail -1; }
# mean brightness of a log line (0 when none)
mean() { echo "$1" | sed -n 's/.* mean=\([0-9.]*\).*/\1/p'; }

INST=${SLY1_READY_INSTALL:-}
[ -n "$INST" ] && INST=$(cd "$INST" && pwd)

# smoke CASE-DIR SRC [FRAMES] [KEYS]: tools/sly1-smoke.sh with the caller's environment; returns its status
smoke() {
    sd=$1; shift
    mkdir -p "$sd"
    "$HERE/sly1-smoke.sh" "$BIN" "$sd" "$@" > "$sd/smoke.txt" 2>&1
}

need_install() {
    [ -n "$INST" ] && return 0
    if [ ! -f "$ISO_USA" ]; then echo "no USA disc ($ISO_USA) and no SLY1_READY_INSTALL"; return 1; fi
    smoke "$OUT/install-usa" "$ISO_USA" 300 "" || { cat "$OUT/install-usa/smoke.txt"; return 1; }
    INST=$(installation "$OUT/install-usa")
}

# ---- regions: install + play into Paris (4200 frames: the logos, the title, a new game, the intro's first talk)
step_regions() {
    for r in usa europe japan korea; do
        case $r in usa) iso=$ISO_USA ;; europe) iso=$ISO_EUROPE ;; japan) iso=$ISO_JAPAN ;; korea) iso=$ISO_KOREA ;; esac
        if [ ! -f "$iso" ]; then skip "regions/$r" "no disc at $iso"; continue; fi
        d=$OUT/regions/$r
        start=$(date +%s)
        if smoke "$d" "$iso"; then
            s=$(lastshot "$d")
            if grep -q 'world=paris\|world=Paris' "$d/shots/log.txt" && [ "$(mean "$s" | cut -d. -f1)" -gt 5 ]; then
                pass "regions/$r" "installed and played into Paris in $(( $(date +%s) - start )) s ($s)"
            else
                fail "regions/$r" "never reached a lit Paris picture (last: $s)"
            fi
            [ $r = usa ] && [ -z "$INST" ] && INST=$(installation "$d")
        else
            fail "regions/$r" "$(tail -1 "$d/smoke.txt")"
        fi
    done
}

# ---- renderers and aspects
step_render() {
    need_install || { fail render "no installation"; return; }
    for renderer in opengl vulkan; do
        for size in 1280x720 3840x1080; do
            c=$renderer-$size; d=$OUT/render/$c
            if SLY1_RENDERER=$renderer SLY1_SMOKE_SIZE=$size smoke "$d" "$INST"; then
                s=$(lastshot "$d")
                case $s in *" ${size} "*) ;; *) fail "render/$c" "the picture is not ${size}: $s"; continue ;; esac
                grep -q "^Vulkan: swapchain" "$d/game.log" && got=vulkan || got=opengl
                [ "$got" = "$renderer" ] || { fail "render/$c" "ran on $got (see $d/game.log)"; continue; }
                pass "render/$c" "$s"
            else
                fail "render/$c" "$(tail -1 "$d/smoke.txt")"
            fi
        done
    done
}

# ---- places: warp (hex level id), a word of the world name the log shows (- any), the context expected, a label.
# The levels: Paris 003, the Hideout 004, Raleigh's boss The Eye of the Storm 108, the turret in Murray's Big Gamble 203,
# Murray's van At the Dog Track 204, the tank (swamp boat) in A Ghastly Voyage 306, the puffer in Piranha Lake 304,
# the jetpack in A Strange Reunion 508. Not reachable by a warp: the rhythm minigame (Mz. Ruby's fight, after a
# walk) and the vaults (a level's clue bottles, then its vault).
PLACES=${SLY1_READY_PLACES:-"003:paris:any:Paris 004:hideout:any:the_Hideout 108:storm:any:Raleigh_boss 203:gamble:turret:turret 204:dog:van:Murrays_van 306:ghastly:tank:tank 304:piranha:puffer:puffer 508:reunion:jetpack:jetpack"}
step_places() {
    need_install || { fail places "no installation"; return; }
    for p in $PLACES; do
        id=${p%%:*}; rest=${p#*:}; world=${rest%%:*}; rest=${rest#*:}; ctx=${rest%%:*}; label=${rest#*:}
        d=$OUT/places/$id
        # the title (Enter twice) into a new game, the warp once Paris plays, then 3600 swaps in the place
        if SLY1_SMOKE_DEBUG=1 SAFEHOUSE_DEBUGMENU="warp=$id@2400,infinite@2500" SLY1_SMOKE_EVERY=300 \
            smoke "$d" "$INST" 6600 "257@1200-1206,257@1500-1506"; then
            s=$(lastshot "$d")
            okctx=$(grep '^shot ' "$d/shots/log.txt" | awk -v c="ctx=$ctx" '$0 ~ c' | tail -1)
            if [ "$world" != - ] && ! echo "$s" | grep -qi "world=[^=]*$world"; then fail "places/$label" "not in $world: $s"
            elif [ -z "$okctx" ]; then fail "places/$label" "never in context $ctx: $s"
            elif [ "$(mean "$s" | cut -d. -f1)" -le 3 ]; then fail "places/$label" "black picture: $s"
            else pass "places/$label" "warp $id: $okctx"; fi
        else
            fail "places/$label" "$(tail -1 "$d/smoke.txt")"
        fi
    done
}

step_pacing() {
    need_install || { fail pacing "no installation"; return; }
    d=$OUT/pacing
    if "$HERE/sly1-pacing.sh" "$BIN" "$d" "$INST" "${SLY1_READY_RATES:-30 60 90 120 144 240 Uncapped}" > "$OUT/pacing.txt" 2>&1; then
        pass pacing "every cap at the game's speed ($d/summary.txt)"
    else
        fail pacing "$(grep FAIL "$OUT/pacing.txt" | head -3 | tr '\n' ' ') ($d/summary.txt)"
    fi
}

step_ps3hd() {
    need_install || { fail ps3hd "no installation"; return; }
    if [ ! -f "$ISO_COLLECTION" ]; then skip ps3hd "no Sly Collection disc at $ISO_COLLECTION"; return; fi
    d=$OUT/ps3hd; mkdir -p "$d/user"
    pack=$d/pack
    if [ ! -f "$pack/Textures.pak" ]; then
        (cd "$BIN" && HOME=$d SDL_AUDIODRIVER=dummy ./Sly1-Release --data-dir "$INST" --user-dir "$d/user" \
            --ps3-hd-textures "$ISO_COLLECTION" "$pack" > "$d/import.log" 2>&1) ||
            { fail ps3hd "the import failed (see $d/import.log)"; return; }
    fi
    if SLY1_SMOKE_ARGS="--texture-pack $pack" smoke "$d/play" "$INST"; then
        n=$(grep -o '[0-9]* textures in .*Textures.pak' "$d/play/game.log" | head -1 | cut -d' ' -f1)
        if [ -n "$n" ]; then pass ps3hd "$(tail -1 "$d/import.log" | cut -c1-60); played with the pack ($n textures loaded)"
        else fail ps3hd "the game did not load the pack (see $d/play/game.log)"; fi
    else
        fail ps3hd "$(tail -1 "$d/play/smoke.txt")"
    fi
}

step_input() {
    need_install || { fail input "no installation"; return; }
    if "$HERE/sly1-input-smoke.sh" "$BIN" "$OUT/input" "$INST" > "$OUT/input.txt" 2>&1; then
        pass input "keyboard, mouse, remap, 7 pad families, 9 icon sets ($OUT/input.txt)"
    else
        fail input "$(grep -i fail "$OUT/input.txt" | head -3 | tr '\n' ' ')"
    fi
}

step_tools() {
    d=$OUT/tools; mkdir -p "$d"
    # a 3 s 440 Hz stereo WAV, a bank from it, the bank played back through the game's music engine
    python3 - "$d/tone.wav" <<'EOF'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b"".join(struct.pack("<hh", v, v) for v in (int(12000 * math.sin(2 * math.pi * 440 * i / 48000)) for i in range(48000 * 3))))
EOF
    if (cd "$BIN" && ./Sly1-Release --make-mus "$d/tone.mus" "$d/tone.wav" --loop none > "$d/make-mus.log" 2>&1) &&
       (cd "$BIN" && SDL_AUDIODRIVER=dummy ./Sly1-Release --export-wav "$d/tone.mus" "$d/back.wav" --seconds 4 > "$d/export-wav.log" 2>&1); then
        peak=$(python3 -c "import sys,wave,struct;w=wave.open(sys.argv[1]);f=w.readframes(w.getnframes());print(max((abs(v) for v in struct.unpack('<%dh'%(len(f)//2),f)),default=0))" "$d/back.wav")
        if [ "${peak:-0}" -gt 1000 ]; then pass tools/music "--make-mus + --export-wav: $(tail -1 "$d/export-wav.log"), peak $peak"
        else fail tools/music "the exported WAV is silent (peak ${peak:-?})"; fi
    else
        fail tools/music "see $d/make-mus.log / $d/export-wav.log"
    fi
    need_install || { fail tools/dump-text "no installation"; return; }
    if (cd "$BIN" && ./Sly1-Release --data-dir "$INST" --user-dir "$d/user" --dump-text "$d/text" > "$d/dump-text.log" 2>&1); then
        pass tools/dump-text "$(tail -1 "$d/dump-text.log")"
    else
        fail tools/dump-text "$(tail -1 "$d/dump-text.log")"
    fi
    # --dump-textures DIR LEVEL: straight to the level, its textures written, the game quits (a hidden window: the
    # smoke hooks with no keys)
    rm -rf "$d/textures"
    if (cd "$BIN" && HOME=$d SDL_AUDIODRIVER=dummy SAFEHOUSE_SLY1_SMOKE="out=$d/dump-shots;every=0;frames=20000"             timeout -s KILL 600 ./Sly1-Release --data-dir "$INST" --user-dir "$d/user" --dump-textures "$d/textures" 103 > "$d/dump-textures.log" 2>&1) &&
       grep -q "Into the Machine dumped" "$d/dump-textures.log"; then
        pass tools/dump-textures "Into the Machine: $(ls "$d/textures" | wc -l) textures"
    else
        fail tools/dump-textures "$(tail -1 "$d/dump-textures.log")"
    fi
}

# dm CASE FRAMES SCRIPT [KEYS]: a smoke run into a new game (Enter twice), Debug mode, the debug menu's test steps
# (SAFEHOUSE_DEBUGMENU: warp=, chk=, watch=, ... safehouse/testaid.cpp); OUT/CASE/game.log holds the watch lines
dm() {
    c=$1; f=$2; script=$3; keys=${4:-"257@1200-1206,257@1500-1506"}; shift 3; [ $# -gt 0 ] && shift
    SLY1_SMOKE_DEBUG=1 SAFEHOUSE_DEBUGMENU="$script" SLY1_SMOKE_EVERY=${SLY1_SMOKE_EVERY:-300} smoke "$OUT/$c" "$INST" "$f" "$keys"
}
# the audio of a smoke run (SLY1_SMOKE_AUDIO: float stereo 48 kHz) analysed by a short Python snippet
pyaudio() { python3 -c "import numpy as np, sys
$1" "$2"; }

step_minigames() {
    need_install || { fail minigames "no installation"; return; }
    # (rhythmauto: the game's test player presses W, A, D or Space for each note in its window: the minigame's own keys)
    if dm minigames/rhythm 4400 "warp=308@2400,infinite@2500,chk=1@3000,rhythmauto=1@3000"; then
        d=$OUT/minigames/rhythm
        hits=$(grep -c '^rhythm: note .* hit' "$d/game.log"); misses=$(grep -c '^rhythm: note .* missed' "$d/game.log")
        if ! grep -q 'ctx=rhythm' "$d/shots/log.txt"; then fail minigames/rhythm "never in the rhythm game: $(lastshot "$d")"
        elif [ "$hits" -lt 8 ] || [ "$misses" -gt 0 ]; then fail minigames/rhythm "W / A / D / Space: $hits notes hit, $misses missed"
        else pass minigames/rhythm "W / A / D / Space hit $hits notes, none missed; $(grep 'ctx=rhythm' "$d/shots/log.txt" | tail -1)"; fi
    else fail minigames/rhythm "$(tail -1 "$OUT/minigames/rhythm/smoke.txt")"; fi
    # Circle at the vault (its button 1710), then the dials: up up, right, up up up, right, up (2-3-1)
    if SLY1_SMOKE_EXTRA="pad=xbox;padbtn=1@3700-3706,11@4000-4006,11@4060-4066,12@4120-4126,11@4180-4186,11@4240-4246,11@4300-4306,12@4360-4366,11@4420-4426" \
        dm minigames/vault 5000 "warp=406@2400,infinite@2500,clues@2950,watch=vault@2990,tpoid=1710@3000"; then
        d=$OUT/minigames/vault
        if grep -q 'watch vault .*state=910/910' "$d/game.log" && grep -q 'ctx=vault' "$d/shots/log.txt"; then pass minigames/vault "Duel by the Dragon's vault opened with 2-3-1 (#45)"
        else fail minigames/vault "the vault did not open (last: $(grep 'watch vault' "$d/game.log" | tail -1))"; fi
    else fail minigames/vault "$(tail -1 "$OUT/minigames/vault/smoke.txt")"; fi
}

step_bugs() {
    need_install || { fail bugs "no installation"; return; }
    # #57: a charm swirling, then sticking, when the player leaves the player list in the frame (the door's exit)
    if dm bugs/charm57 4500 "warp=101@2400,infinite@2500,charm@3000,charmrace@3010,charm@3700,charmrace@3772"; then
        n=$(grep -c '^test: charm .* survived' "$OUT/bugs/charm57/game.log")
        [ "$n" -ge 2 ] && pass bugs/charm57 "both charm races survived ($(grep '^test: charm .* with no player' "$OUT/bugs/charm57/game.log" | tr '\n' ' '))" ||
            fail bugs/charm57 "$n of 2 charm races ran"
    else fail bugs/charm57 "$(tail -1 "$OUT/bugs/charm57/smoke.txt") (#57 crash?)"; fi
    # #11: Back Alley Heist's sprint (vault and key in the save), its hourglass, a checkpoint warp, the finish
    if dm bugs/sprint11 4800 "warp=207@2400,grfls=7@2700,warp=207@2710,watch=mts@3300,tpoid=1870@3350,chk=1@3450,tpoid=1645@3600"; then
        grep -q 'watch mts .*state=3' "$OUT/bugs/sprint11/game.log" && pass bugs/sprint11 "the sprint finished at the finish light" ||
            fail bugs/sprint11 "the sprint never finished ($(grep 'watch mts' "$OUT/bugs/sprint11/game.log" | tail -1))"
    else fail bugs/sprint11 "$(tail -1 "$OUT/bugs/sprint11/smoke.txt")"; fi
    # #48: cane swings (Square) in Into the Machine, an OpenGL error check every frame, then a level load
    swings=$(for f in $(seq 3000 40 4200); do printf '2@%d-%d,' $f $((f + 4)); done)
    for r in opengl vulkan; do
        if SLY1_RENDERER=$r SLY1_SMOKE_EXTRA="pad=xbox;padbtn=${swings%,}" dm bugs/gl48-$r 4800 "warp=103@2400,infinite@2500,watch=gl@2900,warp=101@4300"; then
            log=$OUT/bugs/gl48-$r/game.log
            if grep -q 'watch gl f=\|OpenGL error' "$log"; then fail bugs/gl48-$r "$(grep 'watch gl f=\|OpenGL error' "$log" | head -1)"
            elif grep -q 'watch gl frames=' "$log"; then pass bugs/gl48-$r "$(grep 'watch gl frames=' "$log" | tail -1)"
            else fail bugs/gl48-$r "no GL check ran"; fi
        else fail bugs/gl48-$r "$(tail -1 "$OUT/bugs/gl48-$r/smoke.txt")"; fi
    done
    # #50: the music's beat with Fast held (Triangle, 3600-5600) against the same run without it (Prowling the
    # Grounds)
    a=$OUT/bugs/fast50; ok=1
    for v in off on; do
        btn=""; [ $v = on ] && btn=";padbtn=3@3600-5600"
        SLY1_SMOKE_AUDIO=$a-$v.raw SLY1_SMOKE_EXTRA="pad=xbox$btn" dm bugs/fast50-$v 5800 "warp=101@2400,infinite@2500,fsp=4@2900,watch=music@2900" ||
            { fail bugs/fast50 "$(tail -1 "$OUT/bugs/fast50-$v/smoke.txt")"; ok=0; }
    done
    # (the onset envelope's autocorrelation over the 30 s with Fast on, lags 0.2-3 s, compared between the runs: the
    #  same tempo correlates at about 1.0; the same music 1.5 times faster at about 0.1)
    [ $ok = 1 ] && r=$(python3 -c "import numpy as np, sys
def acf(path):
    x=np.fromfile(path,dtype='<f4').reshape(-1,2).mean(axis=1); n=1024; h=512; dt=h/48000
    F=np.array([np.abs(np.fft.rfft(x[i:i+n]*np.hanning(n))) for i in range(0,len(x)-n,h)])
    o=np.diff(np.log1p(F),axis=0).clip(0).sum(axis=1); T=len(o)*dt
    w=o[int((T-40)/dt):int((T-10)/dt)]; w=w-w.mean(); ac=np.correlate(w,w,'full')[len(w)-1:]
    return ac[int(0.2/dt):int(3/dt)]/ac[0]
print('%.3f' % np.corrcoef(acf(sys.argv[1]), acf(sys.argv[2]))[0,1])" "$a-off.raw" "$a-on.raw")
    [ $ok = 1 ] && rt=$(grep -c 'watch music .*rt=1.50' "$OUT/bugs/fast50-on/game.log") && { awk -v r="$r" -v rt="$rt" 'BEGIN { exit !(rt > 0 && r > 0.8) }' &&
        pass bugs/fast50 "the music's beat without / with Fast correlates $r (Fast on $rt frames)" ||
        fail bugs/fast50 "the music's beat without / with Fast correlates $r (Fast on $rt frames)"; }
    rm -f "$a-off.raw" "$a-on.raw"
    # #51: the Hideout's pause (Start: its Options page) for the last 23 s; the sequencer never paused, the music heard
    a=$OUT/bugs/pause51.raw
    adv=$(for f in $(seq 3000 60 4300); do printf '0@%d-%d,' $f $((f + 4)); done)
    # (Cross through Bentley's welcome, Start ends it, Start again at 6000: the pause)
    if SLY1_SMOKE_AUDIO=$a SLY1_SMOKE_EXTRA="pad=xbox;padbtn=${adv}7@4800-4806,7@6000-6006" dm bugs/pause51 7400 "warp=004@2400,watch=music@2600"; then
        log=$OUT/bugs/pause51/game.log
        paused=$(grep -c 'watch music .*uis=5' "$log"); frozen=$(grep -c 'watch music .*paused=1' "$log")
        rms=$(pyaudio 'x=np.fromfile(sys.argv[1],dtype="<f4"); print("%.4f" % np.sqrt((x[-10*96000:]**2).mean()))' "$a")
        if [ "$paused" -gt 300 ] && [ "$frozen" -eq 0 ] && awk -v r="$rms" 'BEGIN { exit !(r > 0.005) }'; then pass bugs/pause51 "paused $paused frames, the sequencer running, output RMS $rms"
        else fail bugs/pause51 "paused $paused frames, sequencer paused $frozen frames, output RMS $rms"; fi
        rm -f "$a"
    else fail bugs/pause51 "$(tail -1 "$OUT/bugs/pause51/smoke.txt")"; fi
    # #74: Down Home Cooking's roosters (respawn on, each pair placed side by side, Sly flown out of reach) meet and
    # explode, at 60 and 240 FPS
    for rate in 60 240; do
        printf '[Display]\nFrame Rate Limit = %s\nVSync = Off\n' $rate > "$OUT/bugs-roosters-$rate.ini"
        if SLY1_SMOKE_SETTINGS=$OUT/bugs-roosters-$rate.ini dm bugs/roosters74-$rate 4400 "warp=307@2400,infinite@2500,watch=crfod@2600,noclip@2610,fly=0/0/1@2620,fly=0/0/0@2700,crfodk@2700,crfodpair=300@2700"; then
            log=$OUT/bugs/roosters74-$rate/game.log
            t=$(grep 'watch crfod .*alive=' "$log" | tail -1 | sed -n 's/.*touching=\([0-9]*\).*/\1/p')
            if [ "${t:-0}" -gt 0 ] && grep -q 'placed rooster gone' "$log"; then pass bugs/roosters74-$rate "$(grep 'placed rooster gone' "$log" | head -1 | cut -d' ' -f4-) (touching frames $t)"
            else fail bugs/roosters74-$rate "the placed roosters never met ($(grep 'watch crfod .*alive=' "$log" | tail -1))"; fi
        else fail bugs/roosters74-$rate "$(tail -1 "$OUT/bugs/roosters74-$rate/smoke.txt")"; fi
    done
    # #49: High Class Heist's water section (checkpoint 2) for a minute: no clipping, no shrill (high-band) sound
    a=$OUT/bugs/loud49.raw
    if SLY1_SMOKE_AUDIO=$a dm bugs/loud49 5400 "warp=102@2400,infinite@2500,chk=2@3000"; then
        r=$(pyaudio '
x=np.fromfile(sys.argv[1],dtype="<f4").reshape(-1,2); x=x[-60*48000:]; m=x.mean(axis=1); hf=0
for i in range(0,len(m)-3*48000,3*48000):
    S=np.abs(np.fft.rfft(m[i:i+3*48000]))**2; f=np.fft.rfftfreq(3*48000,1/48000); hf=max(hf,S[f>4000].sum()/S.sum())
print("%.3f %.3f" % (np.abs(x).max(), hf))' "$a")
        echo "$r" | awk '{ exit !($1 < 0.95 && $2 < 0.5) }' && pass bugs/loud49 "peak / high-band share: $r" || fail bugs/loud49 "peak / high-band share: $r"
        rm -f "$a"
    else fail bugs/loud49 "$(tail -1 "$OUT/bugs/loud49/smoke.txt")"; fi
    # a level that cannot be loaded (Into the Machine's file missing from a hard-linked copy of the installation)
    broken=$OUT/bugs/broken-install
    rm -rf "$broken"; mkdir -p "$broken" && cp -al "$INST" "$broken/sly1" &&
        find "$broken/sly1/Worlds" -name 'Into the Machine.brx' -exec rm {} +
    SLY1_SMOKE_DEBUG=1 SAFEHOUSE_DEBUGMENU="warp=103@2400" smoke "$OUT/bugs/loadfail" "$broken/sly1" 3600 "257@1200-1206,257@1500-1506"
    if grep -q 'World loading failed' "$OUT/bugs/loadfail/game.log" && grep -q 'exited with 1' "$OUT/bugs/loadfail/smoke.txt"; then
        pass bugs/loadfail "$(grep -o 'World loading failed: .*brx' "$OUT/bugs/loadfail/game.log" | head -1 | sed 's|.*/||')"
    else fail bugs/loadfail "$(tail -1 "$OUT/bugs/loadfail/smoke.txt")"; fi
    rm -rf "$broken"
}

# ---- windows: the Windows build under Proton (optional; SLY1_READY_WINBIN)
step_windows() {
    [ -n "${SLY1_READY_WINBIN:-}" ] && [ -f "${SLY1_READY_WINBIN:-}/Sly1-Release.exe" ] ||
        { skip windows "no SLY1_READY_WINBIN (a MinGW build with CANE_SLY1_SMOKE_HOOKS=ON)"; return; }
    need_install || { fail windows "no installation"; return; }
    wb=$(cd "$SLY1_READY_WINBIN" && pwd); d=$OUT/windows
    mkdir -p "$d"
    x=$("$HERE/sly1-headless-x.sh" "$d/x") || { fail windows "no headless X display: $x"; return; }
    disp=$(echo "$x" | sed -n 's/.*DISPLAY=\(:[0-9]*\).*/\1/p'); xpid=$(echo "$x" | sed -n 's/.*PID=\([0-9]*\).*/\1/p')
    # (the game writes its audio capture beside the data: a hard-linked copy of the installation)
    rm -rf "$d/inst"; mkdir -p "$d/inst" && cp -al "$INST" "$d/inst/sly1"
    tour=
    f=3350
    for p in 5819/0/-300 5835/-2115/-800 5471/1488/-1100 7590/-4186/-1100 8384/3406/-1100 4729/4047/-900 \
             5114/-3863/-950 1094/3326/-800 1182/-3159/-950 6500/0/-900 7800/0/-900 5819/0/-300; do
        tour="$tour,tp=$p@$f"; f=$((f + 150))
    done
    if SLY1_WINE_DISPLAY=$disp SLY1_SMOKE_DEBUG=1 SLY1_SMOKE_EVERY=300 \
       SAFEHOUSE_DEBUGMENU="warp=102@2400,infinite@2500,chk=2@3000,mixtrace=1@3300$tour,mixtrace=0@5300" \
       "$HERE/sly1-wine-smoke.sh" "$wb" "$d/loud49" "$d/inst/sly1" 6600 "257@1200-1206,257@1500-1506" > "$d/loud49.txt" 2>&1 &&
       [ -f "$d/inst/sly1/CameraDiagnostics/eerie_audio_after_saturator.wav" ]; then
        r=$(pyaudio '
import wave
w=wave.open(sys.argv[1]); x=np.frombuffer(w.readframes(w.getnframes()),dtype="<i2").reshape(-1,2)/32768.0
r=max(np.sqrt((x[i:i+48000]**2).mean()) for i in range(0,len(x)-47999,48000))
print("%.1f %.3f %.3f" % (len(x)/48000, np.abs(x).max(), r))' "$d/inst/sly1/CameraDiagnostics/eerie_audio_after_saturator.wav")
        # (seconds, peak, loudest second's RMS: about 0.1 when right; the howl was full scale, RMS 0.85)
        echo "$r" | awk '{ exit !($1 >= 25 && $2 < 0.95 && $3 < 0.4) }' && pass windows/loud49 "s / peak / loudest RMS: $r" ||
            fail windows/loud49 "s / peak / loudest RMS: $r"
    else fail windows/loud49 "$(tail -1 "$d/loud49.txt")"; fi
    mus=$(ls "$INST"/Worlds/Music/*.mus 2>/dev/null | head -1)
    rm -f "$d/export.wav"
    if [ -n "$mus" ] && SLY1_WINE_DISPLAY=$disp SLY1_WINE_TOOL="--export-wav Z:$mus Z:$d/export.wav --seconds 30" \
       "$HERE/sly1-wine-smoke.sh" "$wb" "$d/export" - - > "$d/export.txt" 2>&1; then
        r=$(pyaudio '
import wave
w=wave.open(sys.argv[1]); x=np.frombuffer(w.readframes(w.getnframes()),dtype="<i2")
print("%d %d %d %.1f %d" % (w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()/w.getframerate(), np.abs(x.astype(int)).max()))' "$d/export.wav")
        echo "$r" | awk '{ exit !($1 == 48000 && $2 == 2 && $3 == 2 && $4 >= 10 && $5 > 1000) }' &&
            pass windows/export-wav "rate / channels / bytes / s / peak: $r" || fail windows/export-wav "rate / channels / bytes / s / peak: $r"
    else fail windows/export-wav "$(tail -1 "$d/export.txt")"; fi
    kill "$xpid" 2>/dev/null
    rm -rf "$d/inst"
}

for s in $STEPS; do
    note "== $s ($(date +%H:%M:%S))"
    "step_$s"
done
note "== $fails failed"
[ $fails -eq 0 ]
