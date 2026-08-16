import SwiftUI

/// The "Dashboard" tab: a pure composition view -- it starts no polling
/// of its own. HAL driver install state comes from HALDriverCheck (a
/// one-shot file-existence check, re-checked each time this view
/// appears); Multitrack connection and RVAD device count come from
/// DeviceStore (already populated/refreshed elsewhere); daemon status
/// comes from DaemonController (already polling while this tab -- one of
/// the "relevant tabs" -- is visible).
struct DashboardView: View {
    @EnvironmentObject var deviceStore: DeviceStore
    @EnvironmentObject var daemonController: DaemonController

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Dashboard")
                .font(.title2).bold()
            Text("At-a-glance status of every piece of the RodeCaster Virtual Audio replacement.")
                .font(.callout)
                .foregroundStyle(.secondary)

            VStack(spacing: 10) {
                statusRow(
                    title: "HAL driver installed",
                    detail: HALDriverCheck.isInstalled ? HALDriverCheck.installedPath : "Not found at \(HALDriverCheck.installedPath)",
                    isGood: HALDriverCheck.isInstalled
                )

                statusRow(
                    title: "RodeCaster Pro 2 Multitrack connected",
                    detail: deviceStore.multitrackDevice.map { "\($0.name) -- \($0.channelCount) ch" } ?? "Not found -- is the RodeCaster Pro 2 connected over USB?",
                    isGood: deviceStore.multitrackDevice != nil
                )

                statusRow(
                    title: "All 5 RVAD virtual devices visible",
                    detail: "\(deviceStore.rvadDeviceCount)/5 visible to CoreAudio",
                    isGood: deviceStore.rvadDeviceCount == 5
                )

                statusRow(
                    title: "Router daemon",
                    detail: daemonStatusDetail,
                    isGood: isDaemonRunning
                )
            }

            Spacer()
        }
        .padding(20)
        .frame(minWidth: 480, minHeight: 360)
        .onAppear { deviceStore.refresh() }
    }

    private var isDaemonRunning: Bool {
        if case .running = daemonController.status { return true }
        return false
    }

    private var daemonStatusDetail: String {
        switch daemonController.status {
        case .running(let pid): return "Running (pid \(pid))"
        case .loadedNotRunning(let lastExit): return "Loaded, not currently running (last exit code \(lastExit))"
        case .notInstalled: return "Not installed as a LaunchAgent"
        case .unknown: return "Unknown -- checking..."
        }
    }

    private func statusRow(title: String, detail: String, isGood: Bool) -> some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: isGood ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundStyle(isGood ? .green : .secondary)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.headline)
                Text(detail).font(.callout).foregroundStyle(.secondary)
            }
            Spacer()
        }
        .padding(10)
        .background(Color.gray.opacity(0.06))
        .cornerRadius(8)
    }
}
