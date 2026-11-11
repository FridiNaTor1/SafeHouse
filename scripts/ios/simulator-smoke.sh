#!/usr/bin/env bash
# The iOS app on the simulators (.github/workflows/ios.yml): an iPhone and an iPad simulator each install the app and
# run it three times: --ios-selftest (Sly 2's self tests inside the app, no game data), --ios-extract-smoke (the
# extractor through the launcher's program runner: --help, a missing and a damaged disc image with their reasons in the
# output and in the app log) and --ios-smoke (the launcher draws
# for about 5 s, saves its own screenshot, quits); a simulator screenshot is taken meanwhile. Everything the app wrote
# (Documents: logs, smoke-launcher.png, selftest.log) is copied to OUT/<device>/.
#
#   simulator-smoke.sh SafeHouse.app BUNDLE_ID OUT
set -uo pipefail
APP="$1"; BUNDLE="$2"; OUT="$3"
mkdir -p "$OUT"
failures=0

# Runs the app with these arguments for at most $1 seconds; its console output goes to $2.
launch() {
  local seconds="$1" log="$2" device="$3"; shift 3
  xcrun simctl launch --console-pty --terminate-running-process "$device" "$BUNDLE" "$@" > "$log" 2>&1 &
  local pid=$!
  for _ in $(seq 1 "$seconds"); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
  done
  if kill -0 "$pid" 2>/dev/null; then
    echo "  (still running after ${seconds} s: stopped)" >> "$log"
    xcrun simctl terminate "$device" "$BUNDLE" 2>/dev/null
    kill "$pid" 2>/dev/null
  fi
  wait "$pid" 2>/dev/null
}

device_for() {
  # the newest available device whose name starts with $1 ("iPhone", "iPad")
  xcrun simctl list devices available -j | python3 -c '
import json, sys
prefix = sys.argv[1]
best = None
for runtime, devices in json.load(sys.stdin)["devices"].items():
    if "iOS" not in runtime:
        continue
    for d in devices:
        if d["name"].startswith(prefix) and d.get("isAvailable", True):
            best = (runtime, d["udid"], d["name"])
print(best[1] if best else "")
print(best[2] if best else "", file=sys.stderr)' "$1"
}

for kind in iPhone iPad; do
  device="$(device_for "$kind" 2> "$OUT/$kind-name.txt")"
  if [ -z "$device" ]; then echo "no $kind simulator"; failures=$((failures + 1)); continue; fi
  name="$(cat "$OUT/$kind-name.txt")"
  dir="$OUT/$kind"; mkdir -p "$dir"
  echo "== $kind: $name ($device)"
  xcrun simctl boot "$device" 2>/dev/null
  xcrun simctl bootstatus "$device" -b > /dev/null 2>&1
  xcrun simctl install "$device" "$APP" || { echo "install failed"; failures=$((failures + 1)); continue; }

  echo "-- self tests"
  launch 240 "$dir/selftest-console.txt" "$device" --ios-selftest
  tail -n 30 "$dir/selftest-console.txt"

  echo "-- extractor smoke"
  launch 120 "$dir/extract-console.txt" "$device" --ios-extract-smoke
  tail -n 30 "$dir/extract-console.txt"
  data="$(xcrun simctl get_app_container "$device" "$BUNDLE" data 2>/dev/null)"
  if [ -n "$data" ] && [ -f "$data/Documents/extract-smoke.log" ]; then
    cp "$data/Documents/extract-smoke.log" "$dir/extract-smoke.log"
    cp "$data/Documents/logs/ios.log" "$dir/extract-ios.log" 2>/dev/null
    cat "$dir/extract-smoke.log"
  fi
  grep -q "^0 failed" "$dir/extract-smoke.log" 2>/dev/null || { echo "$kind: extractor smoke did not pass"; failures=$((failures + 1)); }

  echo "-- launcher smoke"
  launch 60 "$dir/smoke-console.txt" "$device" --ios-smoke &
  runner=$!
  sleep 4
  xcrun simctl io "$device" screenshot "$dir/simulator-screenshot.png" > /dev/null 2>&1 || echo "  (no simulator screenshot)"
  wait "$runner"
  tail -n 30 "$dir/smoke-console.txt"

  data="$(xcrun simctl get_app_container "$device" "$BUNDLE" data 2>/dev/null)"
  if [ -n "$data" ] && [ -d "$data/Documents" ]; then
    cp -R "$data/Documents" "$dir/Documents"
    find "$dir/Documents" -maxdepth 3 -type f | head -n 50
  fi
  grep -q "^0 failed" "$dir/Documents/selftest.log" 2>/dev/null || { echo "$kind: self tests did not pass"; failures=$((failures + 1)); }
  [ -f "$dir/Documents/smoke-launcher.png" ] || { echo "$kind: no launcher screenshot from the app"; failures=$((failures + 1)); }
  xcrun simctl shutdown "$device" 2>/dev/null
done
echo "simulator smoke: $failures problem(s)"
exit $((failures > 0))
