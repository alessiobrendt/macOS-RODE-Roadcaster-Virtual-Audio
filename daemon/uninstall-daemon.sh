#!/bin/bash
#
# uninstall-daemon.sh
#
# Stops and removes the rodevad-router per-user LaunchAgent. No sudo
# required (same as install-daemon.sh -- this only ever touches the
# current user's own ~/Library/LaunchAgents/).
#
# Run manually:
#   cd ~/Developer/RodeCasterVirtualAudio
#   ./daemon/uninstall-daemon.sh
#
# Use this to roll back if the router causes any audio problems -- it
# stops audio from being copied into the RodeCaster's Main Multitrack
# device immediately (launchctl unload sends SIGTERM, and the daemon's
# signal handler stops/destroys its IOProcs cleanly before exiting).

set -euo pipefail

LABEL="com.abrendt.rodevad.router"
DEST_PLIST="$HOME/Library/LaunchAgents/${LABEL}.plist"

echo "==> rodevad-router LaunchAgent uninstaller"

if [ ! -f "$DEST_PLIST" ]; then
    echo "==> $DEST_PLIST does not exist -- nothing to remove."
    exit 0
fi

echo "==> Unloading (launchctl unload) -- this stops it immediately..."
launchctl unload "$DEST_PLIST" || true

echo "==> Removing $DEST_PLIST"
rm -f "$DEST_PLIST"

echo "==> Done. rodevad-router is stopped and will not start at next login."
echo "    The RVAD virtual devices and RodeCasterVirtualAudio.driver itself"
echo "    are untouched -- this only removes the routing daemon. To also"
echo "    remove the driver, use ../uninstall.sh."
