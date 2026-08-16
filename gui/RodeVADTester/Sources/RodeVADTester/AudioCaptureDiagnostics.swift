import Foundation
import AppKit

/// Detects and helps fix the single most common "why is there no sound"
/// failure mode for this project: the router daemon starts up perfectly
/// healthy (all devices found, all IOProcs registered, no ERROR lines) but
/// macOS silently delivers ZERO audio to its capture callback instead of an
/// explicit error, because the daemon binary hasn't been granted the
/// relevant privacy permission (Microphone, and/or -- on newer macOS --
/// "Screen & System Audio Recording", which also governs capturing other
/// apps'/system audio via a virtual device the way VirtualDeviceIOProc
/// does). This looks identical to "healthy, nobody's played anything into
/// it yet" from the log alone -- see daemon/rodevad-router.c's NOTE lines
/// -- so this runs an ACTIVE probe instead of relying on the user to
/// notice: play a real test tone into "RVAD System" and watch whether the
/// daemon's own levels file ever registers non-zero signal while it plays.
enum AudioCaptureDiagnosis: Equatable {
    /// The daemon's levels file showed real signal while the test tone was
    /// playing -- audio capture is working. If the user still isn't
    /// hearing anything, the problem is downstream of the daemon (routing
    /// to the Multitrack device, RodeCaster hardware/cabling, app output
    /// device selection), not a permissions issue.
    case audioReceived
    /// The test tone played for its full duration but the daemon's levels
    /// stayed at zero throughout -- the classic signature of a missing
    /// Microphone / Screen & System Audio Recording permission grant for
    /// this specific binary identity.
    case noAudioReceived
    /// Couldn't find "RVAD System" in the current CoreAudio device list --
    /// the driver isn't installed/loaded, so there's nothing to test yet.
    case deviceNotFound
}

enum AudioCaptureDiagnostics {
    private static let probeDeviceName = "RVAD System"
    private static let probeLevelsKey = "system"
    private static let signalThreshold: Float = 0.001

    /// Runs the active probe described above. Always lets the test tone
    /// finish its full `duration` naturally (never cuts it short even once
    /// signal is detected) so the user sees/hears the exact same thing
    /// Channel Test's Play button would produce -- this is deliberately
    /// just a diagnostic wrapper around the existing test-tone flow, not a
    /// separate audio path.
    static func run(duration: Double = 1.5) async -> AudioCaptureDiagnosis {
        guard let devices = try? AudioDeviceLister.listDevices(),
              let device = devices.first(where: { $0.name == probeDeviceName }) else {
            return .deviceNotFound
        }

        async let playTask: Void? = try? TestToneRunner.play(device: device, channel: 1, duration: duration)

        var detected = false
        let deadline = Date().addingTimeInterval(duration + 0.5)
        while Date() < deadline {
            if let level = currentProbeDeviceLevel(), level > signalThreshold {
                detected = true
                break
            }
            try? await Task.sleep(nanoseconds: 80_000_000)
        }
        _ = await playTask

        return detected ? .audioReceived : .noAudioReceived
    }

    nonisolated private static func currentProbeDeviceLevel() -> Float? {
        guard let url = try? ProjectLayout.levelsFile(),
              let data = try? Data(contentsOf: url),
              let text = String(data: data, encoding: .utf8) else {
            return nil
        }
        let (levels, _) = LevelsPoller.parse(text)
        guard let entry = levels.first(where: { $0.key == probeLevelsKey }) else { return nil }
        return max(entry.peakL, entry.peakR)
    }

    // MARK: - One-click links to the exact System Settings panes

    /// Opens System Settings > Privacy & Security > Microphone directly,
    /// via the standard `x-apple.systempreferences:` URL scheme -- never a
    /// custom settings UI built into this app.
    static func openMicrophonePrivacySettings() {
        openPrivacyPane(anchor: "Privacy_Microphone")
    }

    /// Opens System Settings > Privacy & Security > Screen & System Audio
    /// Recording (macOS Sequoia's merged pane; older macOS versions land on
    /// the equivalent "Screen Recording" pane under the same anchor).
    static func openScreenAndSystemAudioRecordingSettings() {
        openPrivacyPane(anchor: "Privacy_ScreenCapture")
    }

    private static func openPrivacyPane(anchor: String) {
        guard let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(anchor)") else { return }
        NSWorkspace.shared.open(url)
    }
}
