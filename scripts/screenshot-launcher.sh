#!/bin/sh
# Renders every launcher screen offscreen with fake data, for review and for
# scripts/check-launcher-screenshots.py.
#   scripts/screenshot-launcher.sh BINARY OUTPUT_DIR [--quick]
# The launcher runs with a throwaway profile (XDG_CONFIG_HOME/XDG_DATA_HOME in
# a temporary folder). It needs a desktop session (Wayland or X11) for its
# OpenGL context; the window stays hidden.
set -eu
if [ $# -lt 2 ]; then
    echo "usage: $0 LAUNCHER OUTPUT_DIR [--quick]" >&2
    exit 2
fi
BIN=$1
OUT=$2
QUICK=${3:-}
mkdir -p "$OUT"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT INT TERM
export XDG_CONFIG_HOME="$T/cfg" XDG_DATA_HOME="$T/data" XDG_STATE_HOME="$T/state" XDG_CACHE_HOME="$T/cache"
FAMILIES="xbox ps nintendo steamdeck steamcontroller keyboard"
[ "$QUICK" = "--quick" ] && FAMILIES="xbox"
shot() {
    name=$1
    family=$2
    shift 2
    PROJECTCANE_TEST_PROMPT_FAMILY=$family "$BIN" --screenshot "$OUT/$name.png" --dump-layout "$OUT/$name.json" "$@"
}
# Every game page state.
for f in $FAMILIES; do
    shot "game-sly1-$f" "$f" --game sly1 --installed-fixture
done
shot game-sly1-notinstalled xbox --game sly1
shot game-sly2-notinstalled xbox --game sly2 --installed-fixture
shot game-sly2-installed xbox --game sly2 --installed-fixture --sly2-installed-fixture
shot game-sly2-ready xbox --game sly2 --installed-fixture --sly2-installed-fixture --sly2-runtime-fixture
shot game-sly2-installing xbox --game sly2 --screen progress-page
for g in sly3 sly4 hackpack; do
    shot "game-$g" xbox --game "$g" --installed-fixture
done
# Install pages and progress.
shot install-sly1-notinstalled xbox --game sly1 --screen install
shot install-sly1-installed xbox --game sly1 --screen install --installed-fixture
shot install-sly2 xbox --game sly2 --screen install --installed-fixture
shot install-progress-sly1 xbox --game sly1 --screen install-progress
shot install-progress-sly2 xbox --game sly2 --screen install-progress --installed-fixture
# Settings and About.
for c in display graphics audio controls interface launcher advanced; do
    shot "settings-$c" xbox --screen "settings:$c" --installed-fixture
done
shot settings-games xbox --screen settings:games --installed-fixture
for c in display graphics audio controls launcher advanced; do
    shot "settings-sly2-$c" xbox --game sly2 --screen "settings:$c" --installed-fixture --sly2-installed-fixture
done
# The Controls page (its controller card and binding icons) for every prompt family.
CONTROL_FAMILIES="xbox ps ps4 ps5 nintendo switch2 gamecube wii wiiu steamdeck steamcontroller steamcontroller2026 keyboard"
[ "$QUICK" = "--quick" ] && CONTROL_FAMILIES="xbox gamecube"
for f in $CONTROL_FAMILIES; do
    shot "settings-sly2-controls-$f" "$f" --game sly2 --screen settings:controls --installed-fixture --sly2-installed-fixture
done
shot about xbox --screen about --installed-fixture
# Sly 3: the notice before Play; the community links under About in the rail.
shot sly3-notice xbox --game sly3 --installed-fixture --screen sly3-notice
shot nav-rail-discord xbox --screen about --installed-fixture --nav b,down
shot nav-rail-patreon xbox --screen about --installed-fixture --nav b,down,right
# Window sizes: Steam Deck, the smallest window, and 1080p.
for z in 1280x800 960x600 1920x1080 2560x1440; do
    shot "size-game-sly1-$z" xbox --game sly1 --installed-fixture --size "$z"
    shot "size-game-sly3-$z" xbox --game sly3 --installed-fixture --size "$z"
    shot "size-install-sly2-$z" xbox --game sly2 --screen install --installed-fixture --size "$z"
    shot "size-settings-$z" xbox --screen settings:display --installed-fixture --size "$z"
    shot "size-about-$z" xbox --screen about --installed-fixture --size "$z"
done
# Controller navigation: back to the rail, down to Sly 2, confirm into its
# page, confirm Install; LB/RB; Nintendo's swapped confirm/back.
shot nav-rail xbox --game sly1 --installed-fixture --nav b
shot nav-rail-down xbox --game sly1 --installed-fixture --nav b,down
shot nav-enter-page xbox --game sly1 --installed-fixture --nav b,down,a
shot nav-open-install xbox --game sly1 --installed-fixture --nav b,down,a,a
shot nav-install-back xbox --game sly2 --screen install --installed-fixture --nav b
shot nav-right-page xbox --game sly1 --installed-fixture --nav b,right
shot nav-left-rail xbox --game sly1 --installed-fixture --nav left
shot nav-settings-rows xbox --screen settings:display --installed-fixture --nav right
shot nav-rb xbox --game sly1 --installed-fixture --nav rb
shot nav-nintendo-enter nintendo --game sly1 --installed-fixture --nav b,down,a,a
shot nav-keyboard-down keyboard --game sly1 --installed-fixture --nav b,down,down
shot nav-settings-row xbox --game sly1 --installed-fixture --nav b,down,down,down,down,down
shot install-path steamdeck --game sly2 --screen install --installed-fixture --nav x
shot binding-capture ps --screen settings:controls --installed-fixture --nav right,down,down,down,down,down,down,a
# The last selected game is remembered (launcher.json "last_game").
mkdir -p "$XDG_CONFIG_HOME/SafeHouse"
printf '{"last_game": "sly3"}\n' > "$XDG_CONFIG_HOME/SafeHouse/launcher.json"
shot remembered-game xbox --installed-fixture
rm "$XDG_CONFIG_HOME/SafeHouse/launcher.json"
echo "Screenshots written to $OUT"
