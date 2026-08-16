import SwiftUI

/// The "Daemon" tab: Start/Stop buttons wired to DaemonController, its
/// derived status label, and -- always shown alongside, never hidden
/// behind the heuristic status -- the raw log tail. `launchctl list`'s
/// output format isn't a reliable enough contract to trust blindly, so
/// the actual log lines stay visible for you to judge yourself.
struct DaemonControlView: View {
    @EnvironmentObject var daemonController: DaemonController

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            header

            statusSection

            if let error = daemonController.lastActionError {
                banner(text: error, systemImage: "exclamationmark.triangle.fill", color: .red)
            }

            controlsSection

            logSection

            Spacer()
        }
        .padding(20)
        .frame(minWidth: 480, minHeight: 360)
        .onAppear { daemonController.startPolling() }
        .onDisappear { daemonController.stopPolling() }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Router Daemon")
                .font(.title2).bold()
            Text("rodevad-router bridges the 5 RVAD virtual devices into the RodeCaster's Main Multitrack hardware. It runs as a per-user LaunchAgent (no sudo). Starting/stopping it here shells out to the same daemon/install-daemon.sh and daemon/uninstall-daemon.sh scripts you could run by hand.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var statusSection: some View {
        HStack(spacing: 10) {
            Image(systemName: statusIcon)
                .foregroundStyle(statusColor)
            Text(statusText)
                .font(.headline)
            Spacer()
            if daemonController.isBusy {
                ProgressView().controlSize(.small)
            }
        }
        .padding(10)
        .background(Color.gray.opacity(0.06))
        .cornerRadius(8)
    }

    private var controlsSection: some View {
        HStack(spacing: 12) {
            Button("Start") {
                Task { await daemonController.start() }
            }
            .disabled(daemonController.isBusy || isRunning)

            Button("Stop", role: .destructive) {
                Task { await daemonController.stop() }
            }
            .disabled(daemonController.isBusy || !isLoaded)

            Button {
                daemonController.refresh()
            } label: {
                Label("Refresh", systemImage: "arrow.clockwise")
            }
            .disabled(daemonController.isBusy)

            Spacer()
        }
    }

    private var logSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Recent log output (stdout + stderr, most recent lines)")
                .font(.caption)
                .foregroundStyle(.secondary)
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    if daemonController.recentLogLines.isEmpty {
                        Text("(no log output yet)")
                            .font(.caption.monospaced())
                            .foregroundStyle(.secondary)
                    } else {
                        ForEach(Array(daemonController.recentLogLines.enumerated()), id: \.offset) { _, line in
                            Text(line)
                                .font(.caption.monospaced())
                                .foregroundStyle(line.contains("ERROR") ? .red : .primary)
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(minHeight: 140)
            .padding(8)
            .background(Color.black.opacity(0.04))
            .cornerRadius(8)
        }
    }

    private var isRunning: Bool {
        if case .running = daemonController.status { return true }
        return false
    }

    private var isLoaded: Bool {
        switch daemonController.status {
        case .running, .loadedNotRunning: return true
        default: return false
        }
    }

    private var statusIcon: String {
        switch daemonController.status {
        case .running: return "checkmark.circle.fill"
        case .loadedNotRunning: return "clock.circle.fill"
        case .notInstalled: return "xmark.circle.fill"
        case .unknown: return "questionmark.circle.fill"
        }
    }

    private var statusColor: Color {
        switch daemonController.status {
        case .running: return .green
        case .loadedNotRunning: return .yellow
        case .notInstalled: return .secondary
        case .unknown: return .secondary
        }
    }

    private var statusText: String {
        switch daemonController.status {
        case .running(let pid): return "Running (pid \(pid))"
        case .loadedNotRunning(let lastExit): return "Loaded, not currently running (last exit code \(lastExit)) -- may be waiting for devices, check the log below"
        case .notInstalled: return "Not installed"
        case .unknown: return "Checking..."
        }
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
