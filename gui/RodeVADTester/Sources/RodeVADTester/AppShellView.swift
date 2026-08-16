import SwiftUI

/// The app's top-level shell: owns the shared @StateObject instances
/// (DeviceStore, DaemonController, ChannelMapStore, RestartController),
/// injects them via .environmentObject so every tab sees the same live
/// state rather than each tab keeping its own disconnected copy, and
/// hosts the TabView plus a toolbar with the donate button.
///
/// LevelsPoller is the one exception: it's still owned/injected here (so
/// its type is known app-wide, same wiring pattern as everything else),
/// but only MetersView ever calls start()/stop() on it -- from its own
/// onAppear/onDisappear -- so it only actively polls state/rodevad-router.levels
/// while the Levels tab specifically is visible, not for the app's whole
/// lifetime.
struct AppShellView: View {
    @StateObject private var deviceStore = DeviceStore()
    @StateObject private var daemonController = DaemonController()
    @StateObject private var channelMapStore = ChannelMapStore()
    @StateObject private var levelsPoller = LevelsPoller()
    @StateObject private var restartController = RestartController()

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

            FixAudioView()
                .tabItem { Label("Fix Audio", systemImage: "stethoscope") }

            RestartView()
                .tabItem { Label("Restart", systemImage: "arrow.triangle.2.circlepath") }
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
        .environmentObject(restartController)
        .frame(minWidth: 560, minHeight: 460)
    }
}
