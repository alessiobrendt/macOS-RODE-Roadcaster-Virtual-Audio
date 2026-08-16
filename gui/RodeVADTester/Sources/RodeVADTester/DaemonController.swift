import Foundation

/// Derived, best-effort status of the rodevad-router LaunchAgent.
///
/// This is a heuristic, not a state machine backed by a reliable signal:
/// `launchctl list <label>`'s output format is informal NeXT-style text,
/// not a documented, stable contract. Treat every case here as "our best
/// guess" -- DaemonControlView always shows the raw log tail alongside
/// this derived status specifically so the heuristic is never the only
/// thing you have to trust.
enum DaemonStatus: Equatable {
    case notInstalled
    case loadedNotRunning(lastExit: Int32)
    case running(pid: Int32)
    case unknown
}

/// Owns starting/stopping the router daemon (by shelling out to the
/// existing, already-verified daemon/install-daemon.sh and
/// daemon/uninstall-daemon.sh -- both scripts have no interactive
/// prompts, confirmed safe to drive via a plain Process with no PTY) and
/// polling its status via `launchctl list` plus the log tail.
@MainActor
final class DaemonController: ObservableObject {
    @Published var status: DaemonStatus = .unknown
    @Published var isBusy = false
    @Published var recentLogLines: [String] = []
    @Published var lastActionError: String?

    private static let label = "com.abrendt.rodevad.router"
    private var pollTimer: Timer?

    // MARK: - Polling (only while a relevant tab is visible)

    func startPolling() {
        refresh()
        pollTimer?.invalidate()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in self.refresh() }
        }
    }

    func stopPolling() {
        pollTimer?.invalidate()
        pollTimer = nil
    }

    func refresh() {
        Task.detached(priority: .utility) {
            let status = await Self.queryLaunchctl()
            let logLines = await DaemonLogTail.tailCombined(maxLines: 200)
            await MainActor.run {
                self.status = status
                self.recentLogLines = logLines
            }
        }
    }

    // MARK: - Start / stop

    /// Runs daemon/install-daemon.sh, which builds (if needed), self-tests,
    /// writes the resolved LaunchAgent plist, and `launchctl load`s it.
    func start() async {
        await runScript(label: "install-daemon.sh") { try ProjectLayout.installDaemonScript() }
    }

    /// Runs daemon/uninstall-daemon.sh, which `launchctl unload`s it and
    /// removes the installed plist.
    func stop() async {
        await runScript(label: "uninstall-daemon.sh") { try ProjectLayout.uninstallDaemonScript() }
    }

    private func runScript(label: String, locate: () throws -> URL) async {
        guard !isBusy else { return }
        isBusy = true
        lastActionError = nil
        defer { isBusy = false }

        do {
            let scriptURL = try locate()
            try await Self.runProcess(executable: scriptURL)
        } catch {
            lastActionError = "\(label) failed: \(error.localizedDescription)"
        }
        refresh()
    }

    private static func runProcess(executable: URL) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            let process = Process()
            process.executableURL = executable
            process.arguments = []

            let stderrPipe = Pipe()
            process.standardError = stderrPipe
            process.standardOutput = Pipe() // discarded; DaemonLogTail reads the daemon's own logs separately

            process.terminationHandler = { finished in
                if finished.terminationStatus == 0 {
                    continuation.resume(returning: ())
                } else {
                    let data = stderrPipe.fileHandleForReading.readDataToEndOfFile()
                    let text = (String(data: data, encoding: .utf8) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
                    let message = "exit code \(finished.terminationStatus)\(text.isEmpty ? "" : ": \(text)")"
                    continuation.resume(throwing: NSError(domain: "DaemonController", code: Int(finished.terminationStatus), userInfo: [NSLocalizedDescriptionKey: message]))
                }
            }

            do {
                try process.run()
            } catch {
                continuation.resume(throwing: error)
            }
        }
    }

    // MARK: - Status via `launchctl list`

    private static func queryLaunchctl() async -> DaemonStatus {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        process.arguments = ["list", label]

        let stdoutPipe = Pipe()
        process.standardOutput = stdoutPipe
        process.standardError = Pipe()

        do {
            try process.run()
        } catch {
            return .unknown
        }
        process.waitUntilExit()

        // `launchctl list <label>` exits non-zero when nothing by that
        // label is currently loaded at all.
        guard process.terminationStatus == 0 else {
            return .notInstalled
        }

        let data = stdoutPipe.fileHandleForReading.readDataToEndOfFile()
        let text = String(data: data, encoding: .utf8) ?? ""
        return parseLaunchctlOutput(text)
    }

    /// Best-effort parse of `launchctl list <label>`'s output. Looks for
    /// lines shaped like `"PID" = 12345;` and `"LastExitStatus" = 0;`
    /// inside the informal NeXT-style property-list text launchctl
    /// prints. This is intentionally forgiving (falls back to `.unknown`-
    /// adjacent states rather than crashing) because this format is not a
    /// documented contract Apple guarantees to keep stable.
    static func parseLaunchctlOutput(_ text: String) -> DaemonStatus {
        var pid: Int32?
        var lastExit: Int32?

        for rawLine in text.split(separator: "\n") {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("\"PID\"") {
                pid = extractIntValue(from: line).map(Int32.init)
            } else if line.hasPrefix("\"LastExitStatus\"") {
                lastExit = extractIntValue(from: line).map(Int32.init)
            }
        }

        if let pid { return .running(pid: pid) }
        return .loadedNotRunning(lastExit: lastExit ?? 0)
    }

    /// Extracts the integer from a line like `"PID" = 12345;`.
    private static func extractIntValue(from line: String) -> Int? {
        guard let eq = line.firstIndex(of: "=") else { return nil }
        let after = line[line.index(after: eq)...]
        let digits = after.filter { $0.isNumber || $0 == "-" }
        return Int(digits)
    }
}
