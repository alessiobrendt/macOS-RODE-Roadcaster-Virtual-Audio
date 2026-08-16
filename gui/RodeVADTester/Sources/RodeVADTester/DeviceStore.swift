import Foundation

@MainActor
final class DeviceStore: ObservableObject {
    @Published var devices: [AudioDevice] = []
    @Published var selectedDeviceID: AudioDevice.ID?
    @Published var loadError: String?
    @Published var isLoading = false

    var selectedDevice: AudioDevice? {
        devices.first { $0.id == selectedDeviceID }
    }

    /// Count of our own "RVAD *" virtual devices currently visible to
    /// CoreAudio -- used by DashboardView to show "5/5 devices visible"
    /// at a glance. Prefix match (not exact name) so it's robust to the
    /// exact 5 names without hardcoding each one here.
    var rvadDeviceCount: Int {
        devices.filter { $0.name.hasPrefix("RVAD ") }.count
    }

    /// The RodeCaster Pro 2's real "Main Multitrack" hardware device, if
    /// currently connected -- matched by UID substring "RODECaster Pro II",
    /// the same convention daemon/rodevad-router.c itself uses (see
    /// FindMultitrackDevice in that file), deliberately NOT by display
    /// name, since RODE/Apple could tweak the human-readable name across
    /// firmware/driver updates without changing the UID pattern.
    var multitrackDevice: AudioDevice? {
        devices.first { $0.uid.contains("RODECaster Pro II") }
    }

    /// The device name we prefer to auto-select the first time the list
    /// loads successfully -- our own virtual driver, if it's installed
    /// and running.
    private static let preferredDeviceName = "RVAD System"

    func refresh() {
        isLoading = true
        loadError = nil
        let previousSelection = selectedDeviceID

        Task.detached(priority: .userInitiated) {
            do {
                let devices = try AudioDeviceLister.listDevices()
                await MainActor.run {
                    self.devices = devices
                    self.isLoading = false

                    // Keep the existing selection if it's still present;
                    // otherwise prefer our own virtual driver; otherwise
                    // fall back to the first device in the list.
                    if let previousSelection, devices.contains(where: { $0.id == previousSelection }) {
                        self.selectedDeviceID = previousSelection
                    } else if let preferred = devices.first(where: { $0.name == Self.preferredDeviceName }) {
                        self.selectedDeviceID = preferred.id
                    } else {
                        self.selectedDeviceID = devices.first?.id
                    }
                }
            } catch {
                await MainActor.run {
                    self.isLoading = false
                    self.loadError = error.localizedDescription
                }
            }
        }
    }
}
