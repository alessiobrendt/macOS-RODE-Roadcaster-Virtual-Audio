import Foundation

/// One-liner, strictly read-only check for whether the HAL driver bundle
/// is installed system-wide. Deliberately provides no install/uninstall
/// action anywhere -- that stays sudo-gated and out of scope for this
/// GUI. Nothing in this app ever invokes the project-root install.sh /
/// uninstall.sh (the sudo-based HAL driver installer/uninstaller); only
/// the per-user daemon/install-daemon.sh and daemon/uninstall-daemon.sh
/// scripts (via DaemonController) are in scope.
enum HALDriverCheck {
    static let installedPath = "/Library/Audio/Plug-Ins/HAL/RodeCasterVirtualAudio.driver"

    static var isInstalled: Bool {
        FileManager.default.fileExists(atPath: installedPath)
    }
}
