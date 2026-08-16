import Foundation
import ServiceManagement

/// Wraps `SMAppService.mainApp` to register/unregister this app as a
/// login item ("Launch at Login"). Available on macOS 13+, matching this
/// project's minimum deployment target -- no new external dependency, no
/// custom LaunchAgent plist of our own for the app itself (that mechanism
/// is only used for the separate rodevad-router daemon).
///
/// Always reflects *actual* registration status (`SMAppService.mainApp.status`)
/// rather than tracking local UI state independently, so it stays correct
/// even if the user changes this via System Settings > General > Login
/// Items directly instead of through this toggle.
///
/// Reliable behavior across rebuilds/relocations wants the app to live in
/// a stable location like /Applications -- this pairs naturally with the
/// .pkg installer (see `make installer`). The toggle is still exposed and
/// functional when running from build/ during development, just
/// documented (here and in the README) as "installed to /Applications"
/// being the fully-supported long-term case, since a login item
/// registered against a `build/` path can end up pointing at a binary
/// that gets rebuilt/moved out from under it.
@MainActor
final class LoginItemController: ObservableObject {
    @Published var isEnabled = false
    @Published var errorMessage: String?

    func refresh() {
        isEnabled = SMAppService.mainApp.status == .enabled
    }

    /// Attempts to register/unregister as requested. On failure, restores
    /// `isEnabled` to whatever the actual status turned out to be (never
    /// silently trusts the optimistic toggle state), and surfaces the
    /// error message for the UI to display inline rather than crashing or
    /// no-opping silently.
    func setEnabled(_ enabled: Bool) {
        errorMessage = nil
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
        } catch {
            errorMessage = error.localizedDescription
        }
        refresh()
    }
}
