import Foundation

/// Finds the `testtone` CLI binary this GUI shells out to.
///
/// The GUI never reimplements audio playback -- every "Play" button in
/// this app runs the already-built-and-verified `build/testtone` binary
/// as a subprocess. That avoids duplicating (and potentially diverging
/// from) the AudioQueue-based playback logic in `tools/testtone.c`, which
/// has already been exercised against real hardware.
///
/// Path resolution itself is delegated to `ProjectLayout` (the single
/// source of truth for every on-disk path this app uses) rather than
/// duplicated here -- this type now just adds the "and is it actually an
/// executable file" existence check on top of `ProjectLayout.testtoneBinary()`.
/// The public `locate() throws -> URL` signature is unchanged, so
/// AudioDevice.swift and TestToneRunner.swift needed zero changes.
enum TestToneLocator {
    enum LocatorError: LocalizedError {
        case notFound(String)
        var errorDescription: String? {
            switch self {
            case .notFound(let detail):
                return "testtone binary not found. \(detail)"
            }
        }
    }

    static func locate() throws -> URL {
        let candidate: URL
        do {
            candidate = try ProjectLayout.testtoneBinary()
        } catch {
            throw LocatorError.notFound(error.localizedDescription)
        }

        guard FileManager.default.isExecutableFile(atPath: candidate.path) else {
            throw LocatorError.notFound("Expected it at \(candidate.path) (derived from the project root ProjectLayout found), but it's not there or not executable. Run `make testtone` (or `make gui`, which builds it first) and try again.")
        }
        return candidate
    }
}
