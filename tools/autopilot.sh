#!/bin/sh
# Plays an autopilot script in the Azahar emulator and collects the screenshots it takes.
#
#   make DEVTOOLS=1 3dsx
#   tools/autopilot.sh tools/autopilot/tour.txt [where the screenshots go, out/dev/shots]
#
# AZAHAR is the emulator and AZAHAR_SD its SD card folder, when they aren't where they usually are. TIMEOUT is how
# many seconds the script may take (120).
set -eu

if [ $# -lt 1 ]; then
    echo "usage: $0 <script> [output folder]" >&2
    exit 2
fi
script=$1
root=$(cd "$(dirname "$0")/.." && pwd)
out=${2:-$root/out/dev/shots}
app=$root/out/dev/Themezer.3dsx
timeout=${TIMEOUT:-120}

if [ "$(uname)" = Darwin ]; then
    # on a Mac it's opened as an app (run directly, it stops at a warning first)
    for candidate in /Applications/Azahar.app "$HOME/Applications/Azahar.app"; do
        [ -d "$candidate" ] && azahar_default=$candidate
    done
    sd_default="$HOME/Library/Application Support/Azahar/sdmc"
else
    sd_default="${XDG_DATA_HOME:-$HOME/.local/share}/azahar-emu/sdmc"
fi
azahar=${AZAHAR:-${azahar_default:-azahar}}
sd=${AZAHAR_SD:-$sd_default}
case "$azahar" in
    *.app) process=$azahar/Contents/MacOS/azahar ;;
    *) process=$azahar ;;
esac

[ -f "$app" ] || { echo "$app is missing: run make DEVTOOLS=1 3dsx" >&2; exit 1; }
# a second emulator doesn't start the app while the first is open
if pgrep -f "$process" > /dev/null 2>&1; then
    echo "Azahar is already open: close it first" >&2
    exit 1
fi
[ -d "$sd" ] || { echo "no emulated SD card at $sd: open Azahar once, or set AZAHAR_SD" >&2; exit 1; }

shots=$sd/3ds/Themezer/autopilot
mkdir -p "$sd/3ds/Themezer"
rm -rf "$shots"
cp "$script" "$sd/3ds/Themezer/autopilot.txt"

case "$azahar" in
    *.app)
        open -n -a "$azahar" --args "$app"
        pid=
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            pid=$(pgrep -n -f "$process" || true)
            [ -n "$pid" ] && break
            sleep 1
        done
        [ -n "$pid" ] || { echo "Azahar didn't start" >&2; exit 1; }
        ;;
    *)
        "$azahar" "$app" > /dev/null 2>&1 &
        pid=$!
        ;;
esac
cleanup() {
    # the emulator doesn't always stop when asked
    kill "$pid" 2> /dev/null || true
    sleep 1
    kill -9 "$pid" 2> /dev/null || true
    rm -f "$sd/3ds/Themezer/autopilot.txt"
}
trap cleanup EXIT INT TERM

waited=0
while [ ! -f "$shots/done" ]; do
    if [ "$waited" -ge "$timeout" ]; then
        echo "the script didn't finish in $timeout seconds" >&2
        exit 1
    fi
    if ! kill -0 "$pid" 2> /dev/null; then
        echo "the emulator closed before the script finished" >&2
        exit 1
    fi
    sleep 1
    waited=$((waited + 1))
done

mkdir -p "$out"
for shot in "$shots"/*.png; do
    [ -f "$shot" ] && cp "$shot" "$out/"
done
ls "$out"
