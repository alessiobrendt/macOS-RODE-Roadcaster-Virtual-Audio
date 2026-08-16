#!/bin/bash
#
# install.sh
#
# Installs RodeCasterVirtualAudio.driver into the system-wide CoreAudio
# HAL plug-in directory and restarts coreaudiod so it gets picked up.
#
# This script uses sudo internally and IS NOT run automatically by any
# tooling -- you must run it yourself, deliberately, from a terminal:
#
#   cd ~/Developer/RodeCasterVirtualAudio
#   make            # build + verify the .driver bundle first
#   ./install.sh
#
# What it does:
#   1. Builds the driver if build/RodeCasterVirtualAudio.driver is missing.
#   2. Copies it to /Library/Audio/Plug-Ins/HAL/ (requires sudo).
#   3. Fixes ownership (root:wheel) and permissions on the installed copy.
#   4. Restarts coreaudiod (sudo killall coreaudiod) so the audio server
#      reloads its plug-ins and picks up the new device.
#
# Safe to re-run: it simply overwrites any previous copy of this driver.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

DRIVER_NAME="RodeCasterVirtualAudio.driver"
BUILD_PATH="build/${DRIVER_NAME}"
DEST_DIR="/Library/Audio/Plug-Ins/HAL"
DEST_PATH="${DEST_DIR}/${DRIVER_NAME}"

echo "==> RodeCasterVirtualAudio installer"

if [ ! -d "$BUILD_PATH" ]; then
    echo "==> Driver bundle not found at $BUILD_PATH, building it now..."
    make
fi

if [ ! -d "$BUILD_PATH" ]; then
    echo "error: build failed, $BUILD_PATH still does not exist" >&2
    exit 1
fi

echo "==> This will copy $BUILD_PATH to $DEST_PATH"
echo "==> and restart coreaudiod (all currently-playing audio will briefly cut out)."
read -r -p "Continue? [y/N] " CONFIRM
case "$CONFIRM" in
    [yY]|[yY][eE][sS]) ;;
    *) echo "Aborted."; exit 1 ;;
esac

echo "==> Copying driver bundle (sudo required)..."
sudo mkdir -p "$DEST_DIR"
sudo rm -rf "$DEST_PATH"
sudo cp -R "$BUILD_PATH" "$DEST_PATH"

echo "==> Fixing ownership and permissions..."
sudo chown -R root:wheel "$DEST_PATH"
sudo chmod -R 755 "$DEST_PATH"

echo "==> Restarting coreaudiod so it reloads HAL plug-ins..."
sudo killall coreaudiod || true

echo "==> Waiting for coreaudiod to come back up..."
sleep 2

echo "==> Done. Verify with:"
echo "      system_profiler SPAudioDataType | grep -A5 'RVAD'"
echo "    or open Audio MIDI Setup.app and look for all 5 of \"RVAD System\", \"RVAD Game\","
echo "    \"RVAD Music\", \"RVAD Virtual A\", and \"RVAD Virtual B\"."
echo ""
echo "==> If the device does NOT appear:"
echo "    - Re-check System Settings > Privacy & Security. Newer macOS versions"
echo "      may show a prompt blocking a newly-added, non-notarized audio driver"
echo "      extension the first time coreaudiod tries to load it -- look for a"
echo "      banner/button there to allow it, then re-run: sudo killall coreaudiod"
echo "    - Check the system log for load errors:"
echo "        log show --predicate 'process == \"coreaudiod\"' --last 2m"
echo "    - Ad-hoc code signing (what this project uses) is normally enough for"
echo "      coreaudiod to load a HAL plug-in locally, but it is NOT a substitute"
echo "      for a real Developer ID signature + notarization. If you plan to use"
echo "      this on multiple Macs or after macOS updates, expect to re-sign it,"
echo "      and consider getting a proper Developer ID certificate long-term."
