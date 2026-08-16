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

    /// Resolution order: (1) embedded inside this running app bundle's
    /// own Contents/MacOS/ (the self-contained, /Applications-installable
    /// case -- see the Makefile's `gui` target, which copies testtone and
    /// rodevad-router in there), (2) the sibling-of-bundle build/<name>
    /// location (the original dev-checkout layout), (3) the dev-mode
    /// CWD-walk fallback already built into projectRoot(). Cases 2 and 3
    /// are really the same code path (buildDirectory() is derived from
    /// projectRoot(), which already does the CWD-walk) -- only case 1 is
    /// new; everything below it is exactly the pre-existing behavior, so
    /// this is fully backward compatible with the original dev workflow.
    private static func embeddedBinary(named name: String) -> URL? {
        let bundleURL = Bundle.main.bundleURL
        guard bundleURL.pathExtension == "app" else { return nil }
        let candidate = bundleURL.appendingPathComponent("Contents/MacOS").appendingPathComponent(name)
        return FileManager.default.isExecutableFile(atPath: candidate.path) ? candidate : nil
    }

    static func testtoneBinary() throws -> URL {
        if let embedded = embeddedBinary(named: "testtone") { return embedded }
        return try buildDirectory().appendingPathComponent("testtone")
    }

    static func routerBinary() throws -> URL {
        if let embedded = embeddedBinary(named: "rodevad-router") { return embedded }
        return try buildDirectory().appendingPathComponent("rodevad-router")
    }

    // MARK: - Daemon scripts (per-user, no sudo -- see daemon/install-daemon.sh)

    static func installDaemonScript() throws -> URL { try daemonDirectory().appendingPathComponent("install-daemon.sh") }
    static func uninstallDaemonScript() throws -> URL { try daemonDirectory().appendingPathComponent("uninstall-daemon.sh") }

    // MARK: - Runtime data directories (state / config / logs)

    /// The stable per-user runtime data location a properly
    /// `install-all.sh`-installed setup uses instead of a dev checkout's
    /// project-relative directories: `~/Library/Application
    /// Support/RodeCasterVirtualAudio/`. This mirrors exactly what
    /// install-all.sh passes to install-daemon.sh's `--working-dir` flag.
    private static var applicationSupportDirectory: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support")
        return base.appendingPathComponent("RodeCasterVirtualAudio")
    }

    /// Resolves one of the 3 runtime data subdirectories (state/config/
    /// logs). Prefers `~/Library/Application Support/RodeCasterVirtualAudio/<name>`
    /// if it already exists on disk -- meaning install-all.sh has set up
    /// a properly installed daemon that writes there -- falling back to
    /// the project-relative `<projectRoot>/<name>` directory otherwise
    /// (the original, still-fully-supported dev-checkout layout the
    /// currently-live daemon setup uses unchanged). This is a pure
    /// existence check, not a hardcoded "are we installed" flag, so it
    /// adapts automatically to whichever setup is actually present.
    private static func runtimeDataDirectory(named name: String) throws -> URL {
        let appSupportCandidate = applicationSupportDirectory.appendingPathComponent(name)
        if FileManager.default.fileExists(atPath: appSupportCandidate.path) {
            return appSupportCandidate
        }
        return try projectRoot().appendingPathComponent(name)
    }

    // MARK: - Config / state files

    /// The live, user-editable channel-map override. May not exist yet --
    /// callers should fall back to channelMapExampleConfig() as a
    /// starting template (see ChannelMapStore.load()).
    static func channelMapConfig() throws -> URL { try runtimeDataDirectory(named: "config").appendingPathComponent("channel-map.conf") }

    /// The tracked, documented template/example (daemon/channel-map.example.conf).
    /// Always project-relative -- this is a source file, not runtime
    /// data, and only exists in a dev checkout, never inside an installed
    /// app bundle or Application Support.
    static func channelMapExampleConfig() throws -> URL { try daemonDirectory().appendingPathComponent("channel-map.example.conf") }

    /// The daemon's live level-meter snapshot file, rewritten atomically
    /// roughly every 75ms while the daemon is running (see
    /// daemon/rodevad-router.c's LevelsWriterThreadMain).
    static func levelsFile() throws -> URL { try runtimeDataDirectory(named: "state").appendingPathComponent("rodevad-router.levels") }

    // MARK: - Logs

    static func stdoutLog() throws -> URL { try runtimeDataDirectory(named: "logs").appendingPathComponent("rodevad-router.out.log") }
    static func stderrLog() throws -> URL { try runtimeDataDirectory(named: "logs").appendingPathComponent("rodevad-router.err.log") }
}
