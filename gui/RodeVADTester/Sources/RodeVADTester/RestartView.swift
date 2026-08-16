import SwiftUI

/// The "Restart" tab: restarts macOS's `coreaudiod` (reloading every HAL
/// audio driver plug-in system-wide, including ours) and then the router
/// daemon -- a heavier hammer than the Daemon tab's own Start/Stop, for
/// when the RVAD devices or routing get stuck in a way a plain daemon
/// restart doesn't fix.
///
/// This is the one action in this whole app that touches audio outside
/// this project's own devices: restarting coreaudiod briefly interrupts
/// ALL system audio and requires an admin password. It is gated behind an
/// explicit confirmation dialog -- never a silent one-click action -- and
/// the actual elevation happens via the standard native
/// `osascript ... with administrator privileges` dialog, not a custom
/// password UI (see RestartController).
struct RestartView: View {
    @EnvironmentObject var daemonController: DaemonController
    @EnvironmentObject var restartController: RestartController
    @State private var showConfirmation = false

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            header

            warningBanner

            statusSection

            Button {
                showConfirmation = true
            } label: {
                Label("Restart coreaudiod + Router Daemon", systemImage: "arrow.triangle.2.circlepath")
            }
            .disabled(restartController.isRunning)
            .confirmationDialog(
                "This will briefly interrupt ALL audio on this Mac -- every app, not just RodeCaster routing -- and requires your admin password. Continue?",
                isPresented: $showConfirmation,
                titleVisibility: .visible
            ) {
                Button("Restart Now", role: .destructive) {
                    Task { await restartController.run(daemonController: daemonController) }
                }
                Button("Cancel", role: .cancel) {}
            }

            Spacer()
        }
        .padding(20)
        .frame(minWidth: 480, minHeight: 360)
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Restart")
                .font(.title2).bold()
            Text("Restarts macOS's core audio subsystem (coreaudiod), which reloads every HAL audio driver on this Mac including RodeCasterVirtualAudio.driver, then restarts the router daemon. Use this if the RVAD devices or routing get stuck in a way the Daemon tab's own Start/Stop doesn't fix.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var warningBanner: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
            VStack(alignment: .leading, spacing: 4) {
                Text("Restarting coreaudiod interrupts ALL audio on this Mac for a moment -- every app, not just this project -- and requires an admin password (a native macOS prompt, never entered into this app directly).")
                Text("This never reinstalls the driver bundle -- it only asks coreaudiod to reload whatever is already installed at /Library/Audio/Plug-Ins/HAL/. Reinstalling is still install.sh's job.")
            }
            .font(.callout)
            .fixedSize(horizontal: false, vertical: true)
        }
        .padding(10)
        .background(Color.orange.opacity(0.12))
        .cornerRadius(8)
    }

    private var statusSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                if restartController.isRunning {
                    ProgressView().controlSize(.small)
                }
                Text("Status: \(phaseText)")
                    .font(.headline)
            }

            if !restartController.stepLog.isEmpty {
                ScrollView {
                    VStack(alignment: .leading, spacing: 2) {
                        ForEach(Array(restartController.stepLog.enumerated()), id: \.offset) { _, line in
                            Text(line)
                                .font(.caption.monospaced())
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(minHeight: 120)
                .padding(8)
                .background(Color.black.opacity(0.04))
                .cornerRadius(8)
            }
        }
    }

    private var phaseText: String {
        switch restartController.phase {
        case .idle: return "Idle"
        case .stoppingDaemon: return "Stopping router daemon..."
        case .restartingCoreAudio: return "Restarting coreaudiod (admin prompt)..."
        case .waitingForDevices: return "Waiting for devices to reappear..."
        case .startingDaemon: return "Starting router daemon..."
        case .done: return "Done"
        case .failed(let message): return "Failed: \(message)"
        }
    }
}
