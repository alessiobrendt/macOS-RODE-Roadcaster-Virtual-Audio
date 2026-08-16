import SwiftUI

/// The app's top-level shell: owns the shared @StateObject instances
/// (DeviceStore, DaemonController, ChannelMapStore), injects them via
/// .environmentObject so every tab sees the same live state rather than
/// each tab keeping its own disconnected copy, and hosts the TabView plus
/// a toolbar with the donate button.
///
/// LevelsPoller is deliberately NOT created/owned here -- it's scoped to
/// just the Levels tab (created as that tab's own @StateObject in
/// MetersView) since it's the one piece of state that only matters, and
/// should only be actively polling, while that specific tab is visible.
struct AppShellView: View {
    @StateObject private var deviceStore = DeviceStore()
    @StateObject private var daemonController = DaemonController()
    @StateObject private var channelMapStore = ChannelMapStore()
    @StateObject private var levelsPoller = LevelsPoller()

    var body: some View {
        TabView {
            DashboardView()
                .tabItem { Label("Dashboard", systemImage: "gauge") }

            ChannelTesterView()
                .tabItem { Label("Channel Test", systemImage: "waveform") }

            MetersView()
                .tabItem { Label("Levels", systemImage: "chart.bar.fill") }

            ChannelMapEditorView()
                .tabItem { Label("Channel Mapping", systemImage: "arrow.left.arrow.right") }

            DaemonControlView()
                .tabItem { Label("Daemon", systemImage: "gearshape.2.fill") }
        }
        .toolbar {
            ToolbarItem(placement: .automatic) {
                DonateButton()
            }
        }
        .environmentObject(deviceStore)
        .environmentObject(daemonController)
        .environmentObject(channelMapStore)
        .environmentObject(levelsPoller)
        .frame(minWidth: 560, minHeight: 460)
    }
}
