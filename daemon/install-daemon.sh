#!/bin/bash
#
# install-daemon.sh
#
# Installs rodevad-router as a PER-USER LaunchAgent (NOT a system-wide
# LaunchDaemon). Unlike install.sh for the HAL driver, this never needs
# sudo -- LaunchAgents live under the current user's own
# ~/Library/LaunchAgents/ and run in the user's own login session.
#
# Run manually, deliberately, from a terminal, once you've already:
#   1. Installed and verified the HAL driver is live (../install.sh,
#      then confirmed "RVAD System" etc. show up in
#      system_profiler SPAudioDataType or Audio MIDI Setup.app).
#   2. Connected the RodeCaster Pro 2 over USB.
#   3. Read the README "Routing daemon" section, especially the safety
#      notes about testing live with your system volume turned down.
#
#   cd ~/Developer/RodeCasterVirtualAudio
#   make daemon              # build + ad-hoc sign + offline self-test
#   ./daemon/install-daemon.sh
#
# What it does:
#   1. Builds rodevad-router if it hasn't been built yet (default mode
#      only -- skipped if --router-bin points somewhere else, see below).
#   2. Creates a logs/ (+ state/, config/) directory for its stdout/stderr
#      (and runtime data) under the working directory.
#   3. Fills in the actual absolute paths into the
#      com.abrendt.rodevad.router.plist template and copies the result to
#      ~/Library/LaunchAgents/com.abrendt.rodevad.router.plist.
#   4. Runs `launchctl load` to start it now and register it to start
#      automatically at every future login.
#
# Optional arguments (both default to today's exact original behavior when
# omitted, so this remains 100% backward compatible with the plain
# `./install-daemon.sh` no-args invocation the currently-live daemon setup
# uses):
#
#   --router-bin <path>    Path to the rodevad-router binary to run.
#                           Default: <project>/build/rodevad-router
#                           (the dev-checkout build). When installed via
#                           the .pkg installer (see ../Makefile's
#                           `installer` target), the postinstall script
#                           uses its own self-contained equivalent of this
#                           logic instead (it runs as root and must target
#                           the console user specifically, a scenario this
#                           plain per-user script isn't designed for) --
#                           but --router-bin/--working-dir remain here as
#                           a general, scriptable capability, e.g. for
#                           manually pointing this script at an
#                           already-installed /Applications/VAD.app
#                           without going through the .pkg.
#   --working-dir <path>   Directory the daemon's relative state/config/
#                           logs paths resolve against (also becomes the
#                           LaunchAgent's WorkingDirectory).
#                           Default: <project> (this checkout's root).
#
# The daemon itself waits (up to ~5 minutes per launch, then lets launchd
# relaunch it) for the 5 RVAD virtual devices and the RodeCaster's
# "Main Multitrack" USB device to be present -- it's safe to load this
# before the RodeCaster is even plugged in.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

ROUTER_BIN="$PROJECT_DIR/build/rodevad-router"
WORKING_DIR="$PROJECT_DIR"
ROUTER_BIN_OVERRIDDEN=""

while [ $# -gt 0 ]; do
    case "$1" in
        --router-bin)
            ROUTER_BIN="$2"
            ROUTER_BIN_OVERRIDDEN=1
            shift 2
            ;;
        --working-dir)
            WORKING_DIR="$2"
            shift 2
            ;;
        *)
            echo "error: unrecognized argument: $1" >&2
            echo "usage: $0 [--router-bin <path>] [--working-dir <path>]" >&2
            exit 2
            ;;
    esac
done

TEMPLATE_PLIST="$SCRIPT_DIR/com.abrendt.rodevad.router.plist"
LABEL="com.abrendt.rodevad.router"
DEST_PLIST="$HOME/Library/LaunchAgents/${LABEL}.plist"
LOG_DIR="$WORKING_DIR/logs"
STDOUT_LOG="$LOG_DIR/rodevad-router.out.log"
STDERR_LOG="$LOG_DIR/rodevad-router.err.log"

echo "==> rodevad-router LaunchAgent installer"
echo "==> This installs a PER-USER agent -- no sudo required."
echo "==> Router binary: $ROUTER_BIN"
echo "==> Working directory (state/config/logs): $WORKING_DIR"
echo ""
echo "!! SAFETY NOTE: the first time you run this with the RodeCaster Pro 2"
echo "!! actually connected, turn your system volume DOWN first. This is the"
echo "!! first time real audio flows through the router into real hardware --"
echo "!! watch/listen for pops, glitches, or feedback loops before trusting it."
echo "!! See README \"Routing daemon\" for the full pre-flight checklist."
echo ""

if [ -z "$ROUTER_BIN_OVERRIDDEN" ] && [ ! -x "$ROUTER_BIN" ]; then
    echo "==> $ROUTER_BIN not found, building it now..."
    make daemon
fi

if [ ! -x "$ROUTER_BIN" ]; then
    echo "error: router binary not found or not executable at $ROUTER_BIN" >&2
    exit 1
fi

echo "==> Running offline self-test one more time before installing..."
"$ROUTER_BIN" --selftest

mkdir -p "$LOG_DIR" "$WORKING_DIR/state" "$WORKING_DIR/config"

echo "==> Writing $DEST_PLIST"
mkdir -p "$HOME/Library/LaunchAgents"
sed -e "s#__ROUTER_BIN_PATH__#${ROUTER_BIN}#g" \
    -e "s#__STDOUT_LOG__#${STDOUT_LOG}#g" \
    -e "s#__STDERR_LOG__#${STDERR_LOG}#g" \
    -e "s#__WORKING_DIR__#${WORKING_DIR}#g" \
    "$TEMPLATE_PLIST" > "$DEST_PLIST"

plutil -lint "$DEST_PLIST"

echo "==> Loading the LaunchAgent (launchctl load)..."
# Unload first in case a stale copy is already loaded from a previous install.
launchctl unload "$DEST_PLIST" >/dev/null 2>&1 || true
launchctl load "$DEST_PLIST"

echo ""
echo "==> Done. rodevad-router is now loaded and will also start automatically"
echo "    at every future login."
echo ""
echo "==> Check it's actually running and what it's doing:"
echo "      launchctl list | grep ${LABEL}"
echo "      tail -f \"$STDOUT_LOG\""
echo "      tail -f \"$STDERR_LOG\""
echo ""
echo "==> IMPORTANT: silence does not necessarily mean it's broken, and it"
echo "    does not necessarily mean it's working either -- actually check the"
echo "    logs above before assuming either way. If the RodeCaster wasn't"
echo "    plugged in yet, it will just be sitting in its wait-for-devices"
echo "    loop (logs a status line every ~10s) until it is."
echo ""
echo "==> To stop and remove it: ./uninstall-daemon.sh"
