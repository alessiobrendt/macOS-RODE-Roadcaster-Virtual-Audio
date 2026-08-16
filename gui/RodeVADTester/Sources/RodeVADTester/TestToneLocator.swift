import Foundation

/// Finds the `testtone` CLI binary this GUI shells out to.
///
/// The GUI never reimplements audio playback -- every "Play" button in
/// this app runs the already-built-and-verified `build/testtone` binary
/// as a subprocess. That avoids duplicating (and potentially diverging
/// from) the AudioQueue-based playback logic in `tools/testtone.c`, which
/// has already been exercised against real hardware.
///
/// Expected layout once built (see the Makefile's `gui` target):
///
///   build/
///     RodeVADTester.app/Contents/MacOS/RodeVADTester   <- this binary
///     testtone                                         <- sibling of the .app
///
/// so the primary lookup is "the directory containing the .app bundle,
/// plus testtone". A couple of fallbacks are included for running the
/// raw (non-bundled) executable during development via `swift run`.
enum TestToneLocator {
    enum LocatorError: LocalizedError {
        case notFound([String])
        var errorDescription: String? {
            switch self {
            case .notFound(let tried):
                return "testtone binary not found. Looked in:\n" + tried.joined(separator: "\n")
            }
        }
    }

    static func locate() throws -> URL {
        var tried: [String] = []
        let fm = FileManager.default

        // 1. Sibling of the .app bundle (the normal, shipped case):
        //    build/RodeVADTester.app -> build/testtone
        let bundleURL = Bundle.main.bundleURL
        if bundleURL.pathExtension == "app" {
            let candidate = bundleURL.deletingLastPathComponent().appendingPathComponent("testtone")
            tried.append(candidate.path)
            if fm.isExecutableFile(atPath: candidate.path) {
                return candidate
            }
        }

        // 2. Same directory as our own executable (covers running the
        //    raw Mach-O directly, outside of any .app wrapper).
        let executableDir = bundleURL.deletingLastPathComponent()
        let sameDirCandidate = executableDir.appendingPathComponent("testtone")
        tried.append(sameDirCandidate.path)
        if fm.isExecutableFile(atPath: sameDirCandidate.path) {
            return sameDirCandidate
        }

        // 3. Development fallback: walk up from the current working
        //    directory (e.g. gui/RodeVADTester when using `swift run`)
        //    looking for a sibling build/testtone at the project root.
        var dir = URL(fileURLWithPath: fm.currentDirectoryPath)
        for _ in 0..<6 {
            let candidate = dir.appendingPathComponent("build/testtone")
            tried.append(candidate.path)
            if fm.isExecutableFile(atPath: candidate.path) {
                return candidate
            }
            dir.deleteLastPathComponent()
        }

        throw LocatorError.notFound(tried)
    }
}
