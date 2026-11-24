#!/bin/sh
# sly1-headless-x.sh DIR : a headless X display for tools/sly1-wine-smoke.sh (Proton's Wine has only winex11).
#
# Starts SteamOS's gamescope with its headless backend (no window, no output: nothing reaches the desktop) and its own
# Xwayland, in a private HOME / XDG profile under DIR, niced; prints "DISPLAY=:N PID=<gamescope pid>" once the display
# is up. Stop it with `kill PID`. Inside a Flatpak sandbox (e.g. a Flatpak editor's terminal) the host's gamescope and
# Xwayland are run through the host's own loader from /run/host (their libraries stay the host's); elsewhere the
# installed ones are used. Needs a Vulkan GPU (gamescope composites with it).
set -u
D=${1:?usage: sly1-headless-x.sh DIR}
mkdir -p "$D/bin" "$D/rt" "$D/home/.config" "$D/home/.local/share" "$D/home/.cache" "$D/home/.local/state"
D=$(cd "$D" && pwd); chmod 700 "$D/rt"
if [ -x /run/host/usr/bin/gamescope ] && [ -f /run/host/usr/lib/ld-linux-x86-64.so.2 ]; then
    H=/run/host/usr
    for b in Xwayland gamescopereaper; do
        printf '#!/bin/sh\nexec %s/lib/ld-linux-x86-64.so.2 --library-path %s/lib %s/bin/%s "$@"\n' "$H" "$H" "$H" "$b" > "$D/bin/$b"
        chmod +x "$D/bin/$b"
    done
    # (the host's Vulkan drivers, by absolute paths the sandbox can open)
    ICD=
    for icd in "$H"/share/vulkan/icd.d/*.x86_64.json; do
        [ -f "$icd" ] || continue
        sed "s|\"library_path\": *\"/usr/|\"library_path\": \"$H/|" "$icd" > "$D/icd-$(basename "$icd")"
        ICD=${ICD:+$ICD:}$D/icd-$(basename "$icd")
    done
    GS="$H/lib/ld-linux-x86-64.so.2 --library-path $H/lib $H/bin/gamescope"
    XW=$D/bin/Xwayland
    PATHX=$D/bin:$PATH
else
    command -v gamescope > /dev/null || { echo "no gamescope"; exit 1; }
    GS=gamescope; XW=$(command -v Xwayland); PATHX=$PATH; ICD=
fi
env -u DISPLAY -u WAYLAND_DISPLAY HOME="$D/home" XDG_CONFIG_HOME="$D/home/.config" XDG_DATA_HOME="$D/home/.local/share" \
    XDG_CACHE_HOME="$D/home/.cache" XDG_STATE_HOME="$D/home/.local/state" XDG_RUNTIME_DIR="$D/rt" PATH="$PATHX" \
    WLR_XWAYLAND="$XW" ${ICD:+VK_ICD_FILENAMES=$ICD} \
    nice -n 19 $GS --backend headless -W 1280 -H 720 -- sleep infinity > "$D/gamescope.log" 2>&1 &
PID=$!
i=0
while [ $i -lt 30 ]; do
    N=$(sed -n 's/.*Starting Xwayland on :\([0-9]*\).*/\1/p' "$D/gamescope.log" | tail -1)
    [ -n "$N" ] && grep -q "xkbcomp\|Xwayland ready\|wlserver: \[xwayland/server.c" "$D/gamescope.log" && break
    sleep 1; i=$((i + 1))
done
kill -0 $PID 2>/dev/null && [ -n "${N:-}" ] || { echo "gamescope did not start (see $D/gamescope.log)"; exit 1; }
sleep 2
echo "DISPLAY=:$N PID=$PID"
