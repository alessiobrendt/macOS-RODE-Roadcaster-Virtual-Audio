import SwiftUI

/// The "Fix Audio" tab: a self-contained diagnose-and-repair flow for the
/// #1 recurring live report on this project -- the router daemon starts
/// cleanly (no ERROR lines, all devices found, all IOProcs registered) yet
/// every channel's level meter stays pinned at 0.000 forever. That symptom
/// is indistinguishable from "healthy, nobody's played anything yet" just
/// by reading the log, so this tab runs an ACTIVE probe (AudioCaptureDiagnostics
/// plays a real test tone into "RVAD System" and watches whether the
/// daemon's own levels file ever reacts) instead of asking the user to
/// puzzle it out themselves.
///
/// The most common real cause is macOS silently withholding the
/// Microphone / "Screen & System Audio Recording" privacy permission from
/// this specific binary identity -- something this app can surface and
/// point at, but cannot grant on the user's behalf (that always requires
/// an explicit human click in System Settings, by design, for any macOS
/// app). So "fix" here means: diagnose clearly, jump straight to the exact
/// System Settings pane instead of making the user hunt for it, then
/// restart the router daemon in one click so a freshly granted permission
/// takes effect immediately instead of requiring a reboot.
struct FixAudioView: View {
    @EnvironmentObject var daemonController: DaemonController
    @EnvironmentObject var deviceStore: DeviceStore

    @State private var isDiagnosing = false
    @State private var diagnosis: AudioCaptureDiagnosis?
    @State private var diagnosisRanOnce = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            header

            if !isDaemonRunning {
                daemonNotRunningBanner
            } else {
                diagnoseSection

                if let diagnosis {
                    resultSection(for: diagnosis)
                }
            }

            Spacer()
        }
        .padding(20)
        .frame(minWidth: 480, minHeight: 360)
        .onAppear { daemonController.startPolling() }
        .onDisappear { daemonController.stopPolling() }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Fix Audio")
                .font(.title2).bold()
            Text("If channels look healthy everywhere else but you're not hearing anything, this checks the single most common cause: macOS silently blocking this app's audio-capture permission after a rebuild, reinstall, or restart -- the router daemon looks perfectly fine in its own log even when this happens.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var isDaemonRunning: Bool {
        if case .running = daemonController.status { return true }
        return false
    }

    private var daemonNotRunningBanner: some View {
        banner(
            text: "The router daemon isn't running, so there's nothing to diagnose yet. Start it from the Daemon tab first, then come back here.",
            systemImage: "xmark.circle.fill",
            color: .secondary
        )
    }

    private var diagnoseSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            Button {
                Task { await runDiagnosis() }
            } label: {
                if isDiagnosing {
                    Label("Playing test tone into RVAD System, listening for signal…", systemImage: "waveform")
                } else {
                    Label(diagnosisRanOnce ? "Diagnose Again" : "Diagnose Audio Capture", systemImage: "stethoscope")
                }
            }
            .disabled(isDiagnosing)

            if isDiagnosing {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("This plays a short, audible test tone through RVAD System -- turn your volume down if you'd rather not hear it.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .padding(10)
        .background(Color.gray.opacity(0.06))
        .cornerRadius(8)
    }

    @ViewBuilder
    private func resultSection(for diagnosis: AudioCaptureDiagnosis) -> some View {
        switch diagnosis {
        case .audioReceived:
            banner(
                text: "Audio is reaching the router daemon correctly. If you're still not hearing anything, the problem is downstream of the daemon -- check the RodeCaster's own routing/cabling, or which output device the app you're testing with is actually using -- not a permissions issue.",
                systemImage: "checkmark.circle.fill",
                color: .green
            )

        case .deviceNotFound:
            banner(
                text: "Couldn't find \"RVAD System\" in the current device list. Check the Dashboard tab -- the driver may not be installed, or the RodeCaster Pro 2 may not be connected.",
                systemImage: "exclamationmark.triangle.fill",
                color: .orange
            )

        case .noAudioReceived:
            VStack(alignment: .leading, spacing: 12) {
                banner(
                    text: "The test tone played but the daemon captured pure silence. macOS never shows an error for this; it just silently delivers empty audio. Confirmed causes on recent macOS: (1) VAD.app's Info.plist missing NSMicrophoneUsageDescription -- TCC hard-refuses the Microphone request with no prompt at all if that's missing; (2) plain ad-hoc code signing, which macOS 26+ rejects for real-time CoreAudio HAL capture even once permission is granted. Both are fixed in current builds -- if you're still seeing this, you're likely on an older build.",
                    systemImage: "exclamationmark.triangle.fill",
                    color: .red
                )

                VStack(alignment: .leading, spacing: 8) {
                    Text("To fix it:").font(.headline)
                    Text("1. Make sure you're on the latest build/release (rebuild from source, or reinstall the latest .pkg) -- this covers the two causes above.")
                        .font(.callout)
                    Text("2. If it's still happening on a current build, open both Privacy panes below and confirm VAD is switched ON in each list, then click \"Restart Router Daemon\" so it re-attempts capture as a freshly authorized process instead of waiting for a full reboot.")
                        .font(.callout)
                    Text("3. Click \"Diagnose Again\" above to confirm it's fixed.")
                        .font(.callout)
                }
                .fixedSize(horizontal: false, vertical: true)

                HStack(spacing: 12) {
                    Button {
                        AudioCaptureDiagnostics.openMicrophonePrivacySettings()
                    } label: {
                        Label("Open Microphone Settings", systemImage: "mic.fill")
                    }

                    Button {
                        AudioCaptureDiagnostics.openScreenAndSystemAudioRecordingSettings()
                    } label: {
                        Label("Open Screen & System Audio Recording Settings", systemImage: "rectangle.on.rectangle")
                    }
                }

                Button {
                    Task { await restartDaemon() }
                } label: {
                    Label("Restart Router Daemon", systemImage: "arrow.clockwise")
                }
                .disabled(daemonController.isBusy)
            }
        }
    }

    private func runDiagnosis() async {
        guard !isDiagnosing else { return }
        isDiagnosing = true
        diagnosis = nil
        let result = await AudioCaptureDiagnostics.run()
        diagnosis = result
        diagnosisRanOnce = true
        isDiagnosing = false
    }

    private func restartDaemon() async {
        await daemonController.stop()
        await daemonController.start()
    }

    private func banner(text: String, systemImage: String, color: Color) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: systemImage).foregroundStyle(color)
            Text(text).font(.callout).fixedSize(horizontal: false, vertical: true)
        }
        .padding(10)
        .background(color.opacity(0.12))
        .cornerRadius(8)
    }
}
