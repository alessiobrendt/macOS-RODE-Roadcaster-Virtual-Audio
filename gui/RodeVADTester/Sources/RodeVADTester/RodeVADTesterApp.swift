import SwiftUI

@main
struct RodeVADTesterApp: App {
    var body: some Scene {
        WindowGroup("RodeCaster Virtual Audio Tester") {
            AppShellView()
        }
        // .contentSize resizability doesn't fit a tabbed control-surface
        // layout well (each tab wants a different natural size); let the
        // window resize freely instead, with AppShellView supplying its
        // own minWidth/minHeight floor.
        .windowResizability(.contentMinSize)
    }
}
