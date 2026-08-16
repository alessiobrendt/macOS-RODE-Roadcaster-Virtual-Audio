#!/bin/bash
#
# uninstall.sh
#
# Removes RodeCasterVirtualAudio.driver from the system-wide CoreAudio
# HAL plug-in directory and restarts coreaudiod.
#
# Run manually, deliberately:
#
#   cd ~/Developer/RodeCasterVirtualAudio
#   ./uninstall.sh
#
# Use this to roll back if the driver causes any audio problems.

set -euo pipefail

DRIVER_NAME="RodeCasterVirtualAudio.driver"
DEST_DIR="/Library/Audio/Plug-Ins/HAL"
DEST_PATH="${DEST_DIR}/${DRIVER_NAME}"

echo "==> RodeCasterVirtualAudio uninstaller"

if [ ! -d "$DEST_PATH" ]; then
    echo "==> $DEST_PATH does not exist -- nothing to remove."
    exit 0
fi

echo "==> This will remove $DEST_PATH and restart coreaudiod"
echo "==> (all currently-playing audio will briefly cut out)."
read -r -p "Continue? [y/N] " CONFIRM
case "$CONFIRM" in
    [yY]|[yY][eE][sS]) ;;
    *) echo "Aborted."; exit 1 ;;
esac

echo "==> Removing driver bundle (sudo required)..."
sudo rm -rf "$DEST_PATH"

echo "==> Restarting coreaudiod so it drops the unloaded plug-in..."
sudo killall coreaudiod || true

echo "==> Waiting for coreaudiod to come back up..."
sleep 2

echo "==> Done. The 5 \"RVAD *\" devices (System, Game, Music, Virtual A, Virtual B)"
echo "    should no longer appear in system_profiler SPAudioDataType or Audio MIDI Setup.app."
echo ""
echo "==> If any app still shows one selected as an input/output device,"
echo "    re-select a real device in that app's audio settings -- removing"
echo "    the driver does not automatically re-point apps that had it chosen."
echo ""
echo "==> Note: this only removes the HAL driver. If daemon/install-daemon.sh"
echo "    was also used to install the routing daemon, run"
echo "    ./daemon/uninstall-daemon.sh separately to stop and remove that too."
