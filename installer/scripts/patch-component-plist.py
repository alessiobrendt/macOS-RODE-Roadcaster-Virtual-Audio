#!/usr/bin/env python3
"""
patch-component-plist.py

Fixes a real installer bug found by the user double-clicking
build/RodeCasterVirtualAudio-Installer.pkg live: Installer.app's standard
LaunchServices-based bundle-identifier matching found this dev checkout's
own build/VAD.app (same bundle identifier, com.abrendt.rodecastervad.gui,
as the payload being installed) and "relocated" the install there instead
of putting it fresh under /Applications -- this is documented, intentional
pkgbuild/Installer.app behavior for updating existing installs of the same
bundle wherever they are, not a bug in Installer.app itself. Because our
postinstall script hardcodes /Applications/VAD.app/Contents/MacOS/rodevad-router
as the embedded router binary's path, the relocation made that path wrong,
and the postinstall script's own fail-loudly check correctly aborted the
install rather than silently proceeding with a broken setup. See git log
for the full root-cause writeup.

The fix is the standard, Apple-documented mechanism for exactly this
problem: set BundleIsRelocatable=false on the VAD.app entry of the
component property list passed to `pkgbuild --component-plist`, which
tells Installer.app "always install this to --install-location, don't
relocate it to match some other copy found elsewhere on disk" --
regardless of any other same-bundle-ID build/VAD.app copies that will
always exist during development and testing of this project.

Usage: patch-component-plist.py <path-to-plist-from-pkgbuild---analyze>

Modifies the plist in place. Matches by RootRelativeBundlePath == "VAD.app"
(rather than assuming an array index/order, which pkgbuild does not
document as stable) so this keeps working even if the payload root's
directory listing order changes.
"""

import plistlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <component-plist-path>", file=sys.stderr)
        return 2

    path = sys.argv[1]

    with open(path, "rb") as f:
        components = plistlib.load(f)

    target_path = "VAD.app"
    matched = False
    for entry in components:
        if entry.get("RootRelativeBundlePath") == target_path:
            entry["BundleIsRelocatable"] = False
            matched = True

    if not matched:
        print(
            f"error: patch-component-plist.py found no component entry with "
            f"RootRelativeBundlePath == {target_path!r} in {path} -- payload "
            f"layout may have changed; this script needs updating to match.",
            file=sys.stderr,
        )
        return 1

    with open(path, "wb") as f:
        plistlib.dump(components, f)

    print(f"patch-component-plist.py: set BundleIsRelocatable=false for {target_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
