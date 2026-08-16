import Foundation

/// Reads the tail of rodevad-router's stdout/stderr log files without
/// loading the whole file into memory -- seeks near the end (bounded by a
/// byte budget) rather than reading from byte 0, so this stays cheap to
/// poll every couple of seconds even if the daemon has been running for
/// days.
enum DaemonLogTail {
    private static let maxBytesToReadPerFile = 64 * 1024 // 64 KB

    /// Reads up to the last `maxLines` lines of `url`. Returns an empty
    /// array (never throws) if the file doesn't exist yet or can't be
    /// read -- a missing log file is a completely normal state (e.g. the
    /// daemon has never been installed/run) and callers shouldn't have to
    /// special-case an error for it.
    static func tail(url: URL, maxLines: Int) -> [String] {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return [] }
        defer { try? handle.close() }

        guard let fileSize = try? handle.seekToEnd() else { return [] }
        guard fileSize > 0 else { return [] }

        let readSize = min(UInt64(maxBytesToReadPerFile), fileSize)
        let startOffset = fileSize - readSize
        guard (try? handle.seek(toOffset: startOffset)) != nil else { return [] }

        guard let data = try? handle.readToEnd(), let text = String(data: data, encoding: .utf8) else {
            return []
        }

        var lines = text.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        // If we didn't start reading from byte 0, the first "line" is
        // very likely a partial fragment of a longer line -- drop it
        // rather than show a truncated mess.
        if startOffset > 0, !lines.isEmpty {
            lines.removeFirst()
        }
        if lines.last == "" { lines.removeLast() } // trailing newline produces an empty final element

        if lines.count > maxLines {
            lines = Array(lines.suffix(maxLines))
        }
        return lines
    }

    /// Tails both the stdout and stderr logs, off the main thread, each
    /// line prefixed with which stream it came from. This does NOT merge
    /// the two streams by timestamp -- they're two independent files with
    /// no shared clock-sync guarantee at this resolution, so lines are
    /// simply stdout-then-stderr, each internally in file order.
    static func tailCombined(maxLines: Int) async -> [String] {
        await Task.detached(priority: .utility) {
            var combined: [String] = []
            if let outURL = try? ProjectLayout.stdoutLog() {
                combined += tail(url: outURL, maxLines: maxLines).map { "[out] \($0)" }
            }
            if let errURL = try? ProjectLayout.stderrLog() {
                combined += tail(url: errURL, maxLines: maxLines).map { "[err] \($0)" }
            }
            return combined
        }.value
    }

    /// True if any recent line in the tail looks like one of
    /// rodevad-router's own "ERROR" log lines -- used by DaemonController
    /// as one signal (not the only one) for its status heuristic.
    static func hasRecentError(in lines: [String]) -> Bool {
        lines.contains { $0.contains("ERROR") }
    }
}
