#!/usr/bin/env bash
#
# Run the Q simulator on a virtual X server, for machines without a
# display. Drive it with simdrive.py (keys, LCD screenshots).
#
#   sim-xvfb.sh start [simulator args]   # default args: --q1 --eff
#   sim-xvfb.sh stop
#
# Uses DISPLAY :77 and keeps PIDs and logs in /tmp/gb-sim/.
#
set -euo pipefail

STATE=/tmp/gb-sim
DISP=:77
UNIX_DIR="$(cd "$(dirname "$0")/../../unix" && pwd)"

stop() {
    for name in sim xvfb; do
        if [ -f "$STATE/$name.pid" ]; then
            kill "$(cat "$STATE/$name.pid")" 2>/dev/null || true
            rm -f "$STATE/$name.pid"
        fi
    done
}

case "${1:-}" in
start)
    shift
    stop
    mkdir -p -m 700 "$STATE"

    # xterm (the simulator's REPL window) needs X bitmap fonts, which Xvfb
    # doesn't find on NixOS by itself
    fp=$(find /nix/store -maxdepth 1 -type d \
            \( -name '*-font-misc-misc-*' -o -name '*-font-cursor-misc-*' \) \
            -exec test -f '{}/share/fonts/X11/misc/fonts.dir' \; -print \
            | sed 's|$|/share/fonts/X11/misc|' | paste -sd, -)

    Xvfb $DISP -screen 0 1024x900x24 -nolisten tcp ${fp:+-fp "$fp"} \
        > "$STATE/xvfb.log" 2>&1 &
    echo $! > "$STATE/xvfb.pid"
    sleep 1

    # NixOS session variables (PYSDL2_DLL_PATH); the file reads unset vars
    if [ -f /etc/set-environment ]; then set +u; source /etc/set-environment; set -u; fi
    cd "$UNIX_DIR"
    [ $# -eq 0 ] && set -- --q1 --eff

    # Under Xvfb the firmware exits at once when started inside a real xterm,
    # so the PATH holds only a stand-in that runs the firmware directly with
    # its console (print output, tracebacks) going to console.log
    mkdir -p "$STATE/bin"
    cat > "$STATE/bin/xterm" <<EOF
#!$BASH
while [ \$# -gt 0 ] && [ "\$1" != -e ]; do shift; done
shift
exec "\$@" > "$STATE/console.log" 2>&1
EOF
    chmod +x "$STATE/bin/xterm"

    DISPLAY=$DISP XDG_RUNTIME_DIR="$STATE" PATH="$STATE/bin" PYTHONUNBUFFERED=1 \
        ../ENV/bin/python simulator.py "$@" > "$STATE/sim.log" 2>&1 &
    echo $! > "$STATE/sim.pid"
    echo "simulator starting on DISPLAY=$DISP, logs: $STATE/sim.log $STATE/console.log"
    ;;
stop)
    stop
    ;;
*)
    echo "usage: $0 start [simulator args] | stop" >&2
    exit 2
    ;;
esac
