import Foundation

/// Single source of truth for every on-disk path this app cares about:
/// the project root itself, plus everything derived from it (logs/,
/// state/, config/, the daemon's install/uninstall scripts, and the
/// build/ output of both the driver's testtone CLI and the router
/// daemon).
///
/// Resolution order (cached after the first successful lookup):
///
///   1. Bundle-relative, the normal shipped case:
///      build/RodeVADTester.app/Contents/MacOS/RodeVADTester -> two
///      directories up from the .app bundle is the project root
///      (.app -> build/ -> project root).
///   2. Same assumption for a raw (non-bundled) executable sitting
///      directly in build/, without the .app wrapper.
///   3. Development fallback: walk up to 6 parent directories from the
///      current working directory (covers `swift run` from
///      gui/RodeVADTester during development) looking for a directory
///      that has both a `Makefile` and a `daemon/` subdirectory -- cheap
///      markers that this is actually the project root, not just some
///      unrelated ancestor.
///
/// Every other file that used to do its own path-walking (originally just
/// TestToneLocator) now delegates here, so there is exactly one
/// implementation of "how do we find the project root" to keep correct.
enum ProjectLayout {
    enum LayoutError: LocalizedError {
        case projectRootNotFound([String])
        var errorDescription: String? {
            switch self {
            case .projectRootNotFound(let tried):
                return "Could not locate the RodeCasterVirtualAudio project root. Looked in:\n" + tried.joined(separator: "\n")
            }
        }
    }

    private static let markerFile = "Makefile"
    private static let markerDirectory = "daemon"

    private static func looksLikeProjectRoot(_ url: URL) -> Bool {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        let hasMarkerFile = fm.fileExists(atPath: url.appendingPathComponent(markerFile).path)
        let hasMarkerDir = fm.fileExists(atPath: url.appendingPathComponent(markerDirectory).path, isDirectory: &isDir) && isDir.boolValue
        return hasMarkerFile && hasMarkerDir
    }

    private static let cachedRoot: Result<URL, Error> = {
        var tried: [String] = []
        let fm = FileManager.default
        let bundleURL = Bundle.main.bundleURL

        // 1. Bundle-relative: build/RodeVADTester.app -> build/ -> project root.
        if bundleURL.pathExtension == "app" {
            let candidate = bundleURL.deletingLastPathComponent().deletingLastPathComponent()
            tried.append(candidate.path)
            if looksLikeProjectRoot(candidate) { return .success(candidate) }
        }

        // 2. Raw executable assumed to sit directly in build/, same
        //    "one more level up" assumption as case 1 without the
        //    .app wrapper.
        let executableDir = bundleURL.deletingLastPathComponent()
        let rawCandidate = executableDir.deletingLastPathComponent()
        tried.append(rawCandidate.path)
        if looksLikeProjectRoot(rawCandidate) { return .success(rawCandidate) }

        // 3. Development fallback: walk up from the current working
        //    directory looking for the project root markers.
        var dir = URL(fileURLWithPath: fm.currentDirectoryPath)
        for _ in 0..<6 {
            tried.append(dir.path)
            if looksLikeProjectRoot(dir) { return .success(dir) }
            dir.deleteLastPathComponent()
        }

        return .failure(LayoutError.projectRootNotFound(tried))
    }()

    static func projectRoot() throws -> URL {
        switch cachedRoot {
        case .success(let url): return url
        case .failure(let error): throw error
        }
    }

    // MARK: - Derived directories

    static func buildDirectory() throws -> URL { try projectRoot().appendingPathComponent("build") }
    static func logsDirectory() throws -> URL { try projectRoot().appendingPathComponent("logs") }
    static func stateDirectory() throws -> URL { try projectRoot().appendingPathComponent("state") }
    static func configDirectory() throws -> URL { try projectRoot().appendingPathComponent("config") }
    static func daemonDirectory() throws -> URL { try projectRoot().appendingPathComponent("daemon") }

    // MARK: - Binaries

    static func testtoneBinary() throws -> URL { try buildDirectory().appendingPathComponent("testtone") }
    static func routerBinary() throws -> URL { try buildDirectory().appendingPathComponent("rodevad-router") }

    // MARK: - Daemon scripts (per-user, no sudo -- see daemon/install-daemon.sh)

    static func installDaemonScript() throws -> URL { try daemonDirectory().appendingPathComponent("install-daemon.sh") }
    static func uninstallDaemonScript() throws -> URL { try daemonDirectory().appendingPathComponent("uninstall-daemon.sh") }

    // MARK: - Config / state files

    /// The live, user-editable channel-map override. May not exist yet --
    /// callers should fall back to channelMapExampleConfig() as a
    /// starting template (see ChannelMapStore.load()).
    static func channelMapConfig() throws -> URL { try configDirectory().appendingPathComponent("channel-map.conf") }

    /// The tracked, documented template/example (daemon/channel-map.example.conf).
    static func channelMapExampleConfig() throws -> URL { try daemonDirectory().appendingPathComponent("channel-map.example.conf") }

    /// The daemon's live level-meter snapshot file, rewritten atomically
    /// roughly every 75ms while the daemon is running (see
    /// daemon/rodevad-router.c's LevelsWriterThreadMain).
    static func levelsFile() throws -> URL { try stateDirectory().appendingPathComponent("rodevad-router.levels") }

    // MARK: - Logs

    static func stdoutLog() throws -> URL { try logsDirectory().appendingPathComponent("rodevad-router.out.log") }
    static func stderrLog() throws -> URL { try logsDirectory().appendingPathComponent("rodevad-router.err.log") }
}
