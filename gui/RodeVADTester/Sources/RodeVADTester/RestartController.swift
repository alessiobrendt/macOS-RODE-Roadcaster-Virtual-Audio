import Foundation

/// Encapsulates the full "restart everything" sequence used by the
/// Restart tab: stop the router daemon, restart macOS's `coreaudiod`
/// (which reloads every HAL audio driver plug-in system-wide, including
/// ours), wait briefly for the virtual devices to reappear, then start
/// the router daemon again.
///
/// This is a real step up in blast radius from anything else in this app:
/// restarting `coreaudiod` briefly interrupts ALL system audio, not just
/// RodeCaster routing, and requires administrator privileges. Elevation is
/// requested for exactly one command (`killall coreaudiod`) via the
/// standard macOS-native `osascript ... with administrator privileges`
/// pattern, which shows the normal native admin-password dialog -- this
/// app never embeds a raw `sudo` call or a custom password UI. Everything
/// else in the sequence (stopping/starting the router daemon) reuses the
/// existing unprivileged per-user LaunchAgent flow via `DaemonController`
/// -- composed, not duplicated.
///
/// This tab never reinstalls or recopies the driver bundle to
/// /Library/Audio/Plug-Ins/HAL/ -- that stays install.sh's job, out of
/// GUI scope. Restarting coreaudiod only makes it *reload* whatever is
/// already installed there.
@MainActor
final class RestartController: ObservableObject {
    enum Phase: Equatable {
        case idle
        case stoppingDaemon
        case restartingCoreAudio
        case waitingForDevices
        case startingDaemon
        case done
        case failed(String)
    }

    @Published var phase: Phase = .idle
    @Published var isRunning = false
    @Published var stepLog: [String] = []

    /// Runs the full sequence. Composes DaemonController for the
    /// stop/start steps (same install-daemon.sh / uninstall-daemon.sh
    /// flow the Daemon tab uses) rather than reimplementing process
    /// spawning here. Each step's outcome is appended to stepLog as it
    /// happens, so the UI can show real progress instead of one opaque
    /// spinner -- and if the admin prompt is cancelled or coreaudiod
    /// fails to restart, the sequence stops cleanly there rather than
    /// blindly continuing to wait-for-devices / start-daemon.
    func run(daemonController: DaemonController) async {
        guard !isRunning else { return }
        isRunning = true
        defer { isRunning = false }

        stepLog = []

        phase = .stoppingDaemon
        appendLog("Stopping the router daemon...")
        await daemonController.stop()
        if let error = daemonController.lastActionError {
            appendLog("Note: stopping the daemon reported: \(error). Continuing anyway -- coreaudiod will be restarted regardless.")
        } else {
            appendLog("Router daemon stopped.")
        }

        phase = .restartingCoreAudio
        appendLog("Requesting administrator privileges to restart coreaudiod (a native macOS password prompt should appear)...")
        do {
            try await Self.restartCoreAudioWithAdministratorPrivileges()
            appendLog("coreaudiod restarted.")
        } catch {
            let message = error.localizedDescription
            appendLog("coreaudiod restart failed or was cancelled: \(message)")
            phase = .failed(message)
            return
        }

        phase = .waitingForDevices
        appendLog("Waiting for the 5 RVAD virtual devices and the RodeCaster Multitrack device to reappear...")
        let reappeared = await Self.waitForDevicesToReappear()
        appendLog(reappeared
            ? "All required devices reappeared."
            : "Devices didn't all reappear within this short wait -- continuing anyway, since rodevad-router has its own longer internal retry loop for exactly this.")

        phase = .startingDaemon
        appendLog("Starting the router daemon...")
        await daemonController.start()
        if let error = daemonController.lastActionError {
            appendLog("Failed to start the router daemon: \(error)")
            phase = .failed(error)
            return
        }
        appendLog("Router daemon started.")

        phase = .done
        appendLog("Restart sequence complete. Check the Daemon tab / logs to confirm everything is healthy.")
    }

    private func appendLog(_ line: String) {
        stepLog.append(line)
    }

    /// Runs exactly one privileged command -- `killall coreaudiod` -- via
    /// `osascript ... do shell script ... with administrator privileges`,
    /// the standard macOS-native elevation pattern: the user sees the
    /// normal native admin-password dialog, never a custom password UI
    /// built into this app. If the user cancels the prompt, osascript
    /// exits non-zero with a "User canceled." message, which this
    /// surfaces as a clear, specific error.
    nonisolated private static func restartCoreAudioWithAdministratorPrivileges() async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
            process.arguments = ["-e", "do shell script \"killall coreaudiod\" with administrator privileges"]

            let stderrPipe = Pipe()
            process.standardError = stderrPipe
            process.standardOutput = Pipe()

            process.terminationHandler = { finished in
                if finished.terminationStatus == 0 {
                    continuation.resume(returning: ())
                    return
                }
                let data = stderrPipe.fileHandleForReading.readDataToEndOfFile()
                var text = (String(data: data, encoding: .utf8) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
                if text.isEmpty {
                    text = "osascript exited with code \(finished.terminationStatus)"
                } else if text.contains("User canceled") || text.contains("-128") {
                    text = "Cancelled -- the admin password prompt was dismissed or no password was provided."
                }
                continuation.resume(throwing: NSError(domain: "RestartController", code: Int(finished.terminationStatus), userInfo: [NSLocalizedDescriptionKey: text]))
            }

            do {
                try process.run()
            } catch {
                continuation.resume(throwing: error)
            }
        }
    }

    /// Polls `testtone --list-machine` (via the existing AudioDeviceLister
    /// -- no separate enumeration logic) a handful of times, a second
    /// apart, checking for all 5 "RVAD " devices and the RodeCaster
    /// Multitrack device. This is a short, best-effort wait: if devices
    /// haven't reappeared by the time it gives up, the router daemon's own
    /// much longer startup retry loop (~5 minutes) is what actually
    /// matters -- this just avoids starting the daemon at the exact
    /// instant coreaudiod is still mid-reload.
    nonisolated private static func waitForDevicesToReappear(maxAttempts: Int = 10, delaySeconds: UInt64 = 1) async -> Bool {
        for _ in 0..<maxAttempts {
            if let devices = try? AudioDeviceLister.listDevices() {
                let rvadCount = devices.filter { $0.name.hasPrefix("RVAD ") }.count
                let hasMultitrack = devices.contains { $0.uid.contains("RODECaster Pro II") }
                if rvadCount == 5 && hasMultitrack {
                    return true
                }
            }
            try? await Task.sleep(nanoseconds: delaySeconds * 1_000_000_000)
        }
        return false
    }
}
