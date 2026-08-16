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
#   1. Builds rodevad-router if it hasn't been built yet.
#   2. Creates a logs/ directory inside this project for its stdout/stderr.
#   3. Fills in this project's actual absolute paths into the
#      com.abrendt.rodevad.router.plist template and copies the result to
#      ~/Library/LaunchAgents/com.abrendt.rodevad.router.plist.
#   4. Runs `launchctl load` to start it now and register it to start
#      automatically at every future login.
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
TEMPLATE_PLIST="$SCRIPT_DIR/com.abrendt.rodevad.router.plist"
LABEL="com.abrendt.rodevad.router"
DEST_PLIST="$HOME/Library/LaunchAgents/${LABEL}.plist"
LOG_DIR="$PROJECT_DIR/logs"
STDOUT_LOG="$LOG_DIR/rodevad-router.out.log"
STDERR_LOG="$LOG_DIR/rodevad-router.err.log"

echo "==> rodevad-router LaunchAgent installer"
echo "==> This installs a PER-USER agent -- no sudo required."
echo ""
echo "!! SAFETY NOTE: the first time you run this with the RodeCaster Pro 2"
echo "!! actually connected, turn your system volume DOWN first. This is the"
echo "!! first time real audio flows through the router into real hardware --"
echo "!! watch/listen for pops, glitches, or feedback loops before trusting it."
echo "!! See README \"Routing daemon\" for the full pre-flight checklist."
echo ""

if [ ! -x "$ROUTER_BIN" ]; then
    echo "==> $ROUTER_BIN not found, building it now..."
    make daemon
fi

echo "==> Running offline self-test one more time before installing..."
"$ROUTER_BIN" --selftest

mkdir -p "$LOG_DIR"

echo "==> Writing $DEST_PLIST"
mkdir -p "$HOME/Library/LaunchAgents"
sed -e "s#__ROUTER_BIN_PATH__#${ROUTER_BIN}#g" \
    -e "s#__STDOUT_LOG__#${STDOUT_LOG}#g" \
    -e "s#__STDERR_LOG__#${STDERR_LOG}#g" \
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
