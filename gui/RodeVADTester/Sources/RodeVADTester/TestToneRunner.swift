import Foundation

enum TestToneRunError: LocalizedError {
    case processFailed(exitCode: Int32, stderr: String)
    case launchFailed(String)

    var errorDescription: String? {
        switch self {
        case .processFailed(let code, let stderr):
            return "testtone exited with code \(code): \(stderr.isEmpty ? "(no output)" : stderr)"
        case .launchFailed(let reason):
            return "Could not launch testtone: \(reason)"
        }
    }
}

/// Runs `testtone --device <uid> --channel <n> --duration <secs>` as a
/// subprocess and waits for it to finish. Selection is always by device
/// UID (not name or index) since that's the one identifier that's
/// unambiguous and stable regardless of what order CoreAudio happens to
/// enumerate devices in during this particular process launch.
enum TestToneRunner {
    static func play(device: AudioDevice, channel: Int, duration: Double) async throws {
        let testtoneURL = try TestToneLocator.locate()

        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            let process = Process()
            process.executableURL = testtoneURL
            process.arguments = [
                "--device", device.uid,
                "--channel", String(channel),
                "--duration", String(duration)
            ]

            let stderrPipe = Pipe()
            process.standardError = stderrPipe
            process.standardOutput = Pipe() // discard stdout, we don't need it

            process.terminationHandler = { finishedProcess in
                let errData = stderrPipe.fileHandleForReading.readDataToEndOfFile()
                let errText = String(data: errData, encoding: .utf8) ?? ""
                if finishedProcess.terminationStatus == 0 {
                    continuation.resume(returning: ())
                } else {
                    continuation.resume(throwing: TestToneRunError.processFailed(
                        exitCode: finishedProcess.terminationStatus,
                        stderr: errText.trimmingCharacters(in: .whitespacesAndNewlines)
                    ))
                }
            }

            do {
                try process.run()
            } catch {
                continuation.resume(throwing: TestToneRunError.launchFailed(error.localizedDescription))
            }
        }
    }
}
