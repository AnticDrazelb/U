#!/usr/bin/env bash
# Runs the PS4 build's staging folder in shadPS4, headless, and plays a script of keyboard actions
# read from stdin, one "<seconds to wait first> <action> [argument]" per line:
#   key <key>       press and release (a tap of a few milliseconds, which a game can miss)
#   tap <key>       press, hold for 0.25 s, release
#   down <key>      press and hold          up <key>    release
#   shot <file>     a screenshot of the whole (virtual) screen
#   waitfor <text>  wait (up to 2 min) until the app's boot-trace.log has that text
# shadPS4's keyboard: N is Cross, Enter OPTIONS, Space the touchpad, the arrows the D-pad, W A S D
# the left stick, X and M L3 and R3 (its user/input_config/default.ini).
#   SHADPS4  the shadPS4 binary (default /root/emu/shadPS4/build/shadps4)
#   RUN      the folder shadPS4 runs in (default ~/shad-run): its user/data is the app's /data,
#            cleared of eden-ps4 first; the game goes to RUN/game
#   STAGE    the staging folder (default ~/eden-ps4-build/stage)
# Example: start Breakout, let it draw, screenshot it:
#   printf '%s\n' "0 waitfor start page" "0 tap n" "0 waitfor first frame" "2 shot /tmp/b.png" |
#       eden-ps4/shadps4-play.sh
# Needs xvfb-run, xdotool and ImageMagick's import.
set -u
SHADPS4="${SHADPS4:-/root/emu/shadPS4/build/shadps4}"
RUN="${RUN:-$HOME/shad-run}"
STAGE="${STAGE:-$HOME/eden-ps4-build/stage}"

if [ -z "${EDEN_PS4_IN_XVFB:-}" ]; then
    rm -rf "$RUN/game" "$RUN/user/data/eden-ps4"
    mkdir -p "$RUN/game" "$RUN/user"
    cp -r "$STAGE/." "$RUN/game/"
    rm -rf "$RUN/game/sce_module"
    EDEN_PS4_IN_XVFB=1 exec xvfb-run -a -s "-screen 0 1920x1080x24" "$0"
fi

trace="$RUN/user/data/eden-ps4/boot-trace.log"
cd "$RUN"
SDL_AUDIODRIVER=dummy "$SHADPS4" "$RUN/game/eboot.bin" > "$RUN/shadps4.log" 2>&1 &
pid=$!
win=""
while read -r wait action arg; do
    sleep "$wait"
    if [ -z "$win" ]; then
        win="$(xdotool search --name 'shadPS4' 2>/dev/null | head -1)"
        [ -n "$win" ] && xdotool windowfocus "$win" 2>/dev/null
    fi
    case "$action" in
    key) xdotool key --delay 120 "$arg" ;;
    tap) xdotool keydown "$arg"; sleep 0.25; xdotool keyup "$arg" ;;
    down) xdotool keydown "$arg" ;;
    up) xdotool keyup "$arg" ;;
    shot) import -window root "$arg" ;;
    waitfor)
        for _ in $(seq 600); do
            grep -q "$arg" "$trace" 2>/dev/null && break
            sleep 0.2
        done
        ;;
    esac
    echo "$(date +%T) $action $arg"
done
kill $pid 2>/dev/null
wait $pid 2>/dev/null
echo "the app's files: $RUN/user/data/eden-ps4"
